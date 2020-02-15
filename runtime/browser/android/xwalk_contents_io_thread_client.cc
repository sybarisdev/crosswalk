// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/android/xwalk_contents_io_thread_client.h"
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/android/jni_weak_ref.h"
#include "base/lazy_instance.h"
#include "base/synchronization/lock.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/resource_request_info.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/resource_type.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/url_request.h"
#include "url/gurl.h"
#include "xwalk/runtime/android/core_refactor/xwalk_refactor_native_jni/XWalkContentsIoThreadClient_jni.h"
#include "xwalk/runtime/browser/android/xwalk_web_resource_response_impl.h"

using base::android::AttachCurrentThread;
using base::android::ConvertUTF8ToJavaString;
using base::android::JavaRef;
using base::android::ScopedJavaLocalRef;
using base::android::ToJavaArrayOfStrings;
using base::LazyInstance;
using content::BrowserThread;
using content::RenderFrameHost;
using content::WebContents;
using std::map;
using std::pair;
using std::string;
using std::vector;

namespace xwalk {

namespace {

struct IoThreadClientData {
  bool pending_association;
  JavaObjectWeakGlobalRef io_thread_client;

  IoThreadClientData();
};

IoThreadClientData::IoThreadClientData()
    : pending_association(false) {
}

typedef map<pair<int, int>, IoThreadClientData> RenderFrameHostToIoThreadClientType;

typedef pair<base::flat_set<RenderFrameHost*>, IoThreadClientData> HostsAndClientDataPair;

// When browser side navigation is enabled, RenderFrameIDs do not have
// valid render process host and render frame ids for frame navigations.
// We need to identify these by using FrameTreeNodeIds. Furthermore, we need
// to keep track of which RenderFrameHosts are associated with each
// FrameTreeNodeId, so we know when the last RenderFrameHost is deleted (and
// therefore the FrameTreeNodeId should be removed).
typedef map<int, HostsAndClientDataPair> FrameTreeNodeToIoThreadClientType;

static pair<int, int> GetRenderFrameHostIdPair(RenderFrameHost* rfh) {
  return pair<int, int>(rfh->GetProcess()->GetID(), rfh->GetRoutingID());
}

// RfhToIoThreadClientMap -----------------------------------------------------
class RfhToIoThreadClientMap {
 public:
  static RfhToIoThreadClientMap* GetInstance();
  void Set(pair<int, int> rfh_id, const IoThreadClientData& client);
  bool Get(pair<int, int> rfh_id, IoThreadClientData* client);

  bool Get(int frame_tree_node_id, IoThreadClientData* client);

  // Prefer to call these when RenderFrameHost* is available, because they
  // update both maps at the same time.
  void Set(RenderFrameHost* rfh, const IoThreadClientData& client);
  void Erase(RenderFrameHost* rfh);

 private:
  base::Lock map_lock_;
  // We maintain two maps simultaneously so that we can always get the correct
  // IoThreadClientData, even when only HostIdPair or FrameTreeNodeId is
  // available.
  RenderFrameHostToIoThreadClientType rfh_to_io_thread_client_;
  FrameTreeNodeToIoThreadClientType frame_tree_node_to_io_thread_client_;
};

// static
LazyInstance<RfhToIoThreadClientMap>::DestructorAtExit g_instance_ =
LAZY_INSTANCE_INITIALIZER;

// static
RfhToIoThreadClientMap* RfhToIoThreadClientMap::GetInstance() {
  return g_instance_.Pointer();
}

void RfhToIoThreadClientMap::Set(pair<int, int> rfh_id, const IoThreadClientData& client) {
  base::AutoLock lock(map_lock_);
  rfh_to_io_thread_client_[rfh_id] = client;
}

bool RfhToIoThreadClientMap::Get(pair<int, int> rfh_id, IoThreadClientData* client) {
  base::AutoLock lock(map_lock_);
  RenderFrameHostToIoThreadClientType::iterator iterator = rfh_to_io_thread_client_.find(rfh_id);
  if (iterator == rfh_to_io_thread_client_.end())
    return false;

  *client = iterator->second;
  return true;
}

bool RfhToIoThreadClientMap::Get(int frame_tree_node_id, IoThreadClientData* client) {
  base::AutoLock lock(map_lock_);
  FrameTreeNodeToIoThreadClientType::iterator iterator = frame_tree_node_to_io_thread_client_.find(frame_tree_node_id);
  if (iterator == frame_tree_node_to_io_thread_client_.end())
    return false;

  *client = iterator->second.second;
  return true;
}

void RfhToIoThreadClientMap::Set(RenderFrameHost* rfh, const IoThreadClientData& client) {
  int frame_tree_node_id = rfh->GetFrameTreeNodeId();
  pair<int, int> rfh_id = GetRenderFrameHostIdPair(rfh);
  base::AutoLock lock(map_lock_);

  // If this FrameTreeNodeId already has an associated IoThreadClientData, add
  // this RenderFrameHost to the hosts set (it's harmless to overwrite the
  // IoThreadClientData). Otherwise, operator[] creates a new map entry and we
  // add this RenderFrameHost to the hosts set and insert |client| in the pair.
  HostsAndClientDataPair& current_entry = frame_tree_node_to_io_thread_client_[frame_tree_node_id];
  current_entry.second = client;
  current_entry.first.insert(rfh);

  // Always add the entry to the HostIdPair map, since entries are 1:1 with
  // RenderFrameHosts.
  rfh_to_io_thread_client_[rfh_id] = client;
}

void RfhToIoThreadClientMap::Erase(RenderFrameHost* rfh) {
  int frame_tree_node_id = rfh->GetFrameTreeNodeId();
  pair<int, int> rfh_id = GetRenderFrameHostIdPair(rfh);
  base::AutoLock lock(map_lock_);
  HostsAndClientDataPair& current_entry = frame_tree_node_to_io_thread_client_[frame_tree_node_id];
  size_t num_erased = current_entry.first.erase(rfh);
  DCHECK(num_erased == 1);
  // Only remove this entry from the FrameTreeNodeId map if there are no more
  // live RenderFrameHosts.
  if (current_entry.first.empty()) {
    frame_tree_node_to_io_thread_client_.erase(frame_tree_node_id);
  }

  // Always safe to remove the entry from the HostIdPair map, since entries are
  // 1:1 with RenderFrameHosts.
  rfh_to_io_thread_client_.erase(rfh_id);
}

// ClientMapEntryUpdater ------------------------------------------------------

class ClientMapEntryUpdater : public content::WebContentsObserver {
 public:
  ClientMapEntryUpdater(JNIEnv* env,
                        WebContents* web_contents,
                        jobject jdelegate);

  void RenderFrameCreated(RenderFrameHost* render_frame_host) override;
  void RenderFrameDeleted(RenderFrameHost* render_frame_host) override;
  void WebContentsDestroyed() override;

 private:
  JavaObjectWeakGlobalRef jdelegate_;
};

ClientMapEntryUpdater::ClientMapEntryUpdater(JNIEnv* env,
                                             WebContents* web_contents,
                                             jobject jdelegate)
    : content::WebContentsObserver(web_contents), jdelegate_(env, jdelegate) {
  DCHECK(web_contents);
  DCHECK(jdelegate);

  if (web_contents->GetMainFrame())
    RenderFrameCreated(web_contents->GetMainFrame());
}

void ClientMapEntryUpdater::RenderFrameCreated(RenderFrameHost* rfh) {
  LOG(INFO) << "iotto " << __func__ << " renderFrameHost=" << rfh->GetFrameName() << " node_id=" << rfh->GetFrameTreeNodeId();
  IoThreadClientData client_data;
  client_data.io_thread_client = jdelegate_;
  client_data.pending_association = false;
  RfhToIoThreadClientMap::GetInstance()->Set(rfh, client_data);
}

void ClientMapEntryUpdater::RenderFrameDeleted(RenderFrameHost* rfh) {
  LOG(INFO) << "iotto " << __func__ << " renderFrameHost=" << rfh->GetFrameName() << " node_id=" << rfh->GetFrameTreeNodeId();
  RfhToIoThreadClientMap::GetInstance()->Erase(rfh);
}

void ClientMapEntryUpdater::WebContentsDestroyed() {
  delete this;
}


struct WebResourceRequest {
  ScopedJavaLocalRef<jstring> jstring_url;
  bool is_main_frame;
  bool has_user_gesture;
  ScopedJavaLocalRef<jstring> jstring_method;
  ScopedJavaLocalRef<jobjectArray> jstringArray_header_names;
  ScopedJavaLocalRef<jobjectArray> jstringArray_header_values;

  WebResourceRequest(JNIEnv* env, const net::URLRequest* request)
      : jstring_url(ConvertUTF8ToJavaString(env, request->url().spec())),
        jstring_method(ConvertUTF8ToJavaString(env, request->method())) {
    content::ResourceRequestInfo* info = content::ResourceRequestInfo::ForRequest(request);
    is_main_frame =
        info && info->GetResourceType() == content::ResourceType::kMainFrame;
    has_user_gesture = info && info->HasUserGesture();

    vector<string> header_names;
    vector<string> header_values;
    net::HttpRequestHeaders headers;
    if (!request->GetFullRequestHeaders(&headers))
      headers = request->extra_request_headers();
    net::HttpRequestHeaders::Iterator headers_iterator(headers);
    while (headers_iterator.GetNext()) {
      header_names.push_back(headers_iterator.name());
      header_values.push_back(headers_iterator.value());
    }
    jstringArray_header_names = ToJavaArrayOfStrings(env, header_names);
    jstringArray_header_values = ToJavaArrayOfStrings(env, header_values);
  }
};

}  // namespace

// XWalkContentsIoThreadClient -------------------------------------------

// static
std::unique_ptr<XWalkContentsIoThreadClient> XWalkContentsIoThreadClient::FromID(int render_process_id,
                                                                                 int render_frame_id) {
  pair<int, int> rfh_id(render_process_id, render_frame_id);
  IoThreadClientData client_data;
  if (!RfhToIoThreadClientMap::GetInstance()->Get(rfh_id, &client_data))
    return std::unique_ptr<XWalkContentsIoThreadClient>();

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> java_delegate = client_data.io_thread_client.get(env);
  DCHECK(!client_data.pending_association || java_delegate.is_null());
  return std::unique_ptr<XWalkContentsIoThreadClient>(
      new XWalkContentsIoThreadClient(client_data.pending_association, java_delegate));
}

std::unique_ptr<XWalkContentsIoThreadClient> XWalkContentsIoThreadClient::FromID(int frame_tree_node_id) {
  IoThreadClientData client_data;
  if (!RfhToIoThreadClientMap::GetInstance()->Get(frame_tree_node_id, &client_data))
    return std::unique_ptr<XWalkContentsIoThreadClient>();

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jobject> java_delegate = client_data.io_thread_client.get(env);
  DCHECK(!client_data.pending_association || java_delegate.is_null());
  return std::unique_ptr<XWalkContentsIoThreadClient>(
      new XWalkContentsIoThreadClient(client_data.pending_association, java_delegate));
}

// static
void XWalkContentsIoThreadClient::SubFrameCreated(int render_process_id,
                                                  int parent_render_frame_id,
                                                  int child_render_frame_id) {
  pair<int, int> parent_rfh_id(render_process_id, parent_render_frame_id);
  pair<int, int> child_rfh_id(render_process_id, child_render_frame_id);
  IoThreadClientData client_data;
  if (!RfhToIoThreadClientMap::GetInstance()->Get(parent_rfh_id,
                                                  &client_data)) {
    NOTREACHED();
    return;
  }

  RfhToIoThreadClientMap::GetInstance()->Set(child_rfh_id, client_data);
}


// static
void XWalkContentsIoThreadClient::RegisterPendingContents(
    WebContents* web_contents) {
  IoThreadClientData client_data;
  client_data.pending_association = true;
  RfhToIoThreadClientMap::GetInstance()->Set(
      GetRenderFrameHostIdPair(web_contents->GetMainFrame()), client_data);
}

// static
void XWalkContentsIoThreadClient::Associate(WebContents* web_contents, const JavaRef<jobject>& jclient) {
  JNIEnv* env = AttachCurrentThread();
  // The ClientMapEntryUpdater lifespan is tied to the WebContents.
  new ClientMapEntryUpdater(env, web_contents, jclient.obj());
}

XWalkContentsIoThreadClient::XWalkContentsIoThreadClient(bool pending_association, const JavaRef<jobject>& obj)
    : pending_association_(pending_association),
      java_object_(obj) {
}

XWalkContentsIoThreadClient::~XWalkContentsIoThreadClient() {
}

bool XWalkContentsIoThreadClient::PendingAssociation() const {
  return pending_association_;
}

XWalkContentsIoThreadClient::CacheMode
XWalkContentsIoThreadClient::GetCacheMode() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (java_object_.is_null())
    return XWalkContentsIoThreadClient::LOAD_DEFAULT;

  JNIEnv* env = AttachCurrentThread();
  return static_cast<XWalkContentsIoThreadClient::CacheMode>(
      Java_XWalkContentsIoThreadClient_getCacheMode(
          env, java_object_));
}

std::unique_ptr<XWalkWebResourceResponse>
XWalkContentsIoThreadClient::ShouldInterceptRequest(
    const GURL& location,
    const net::URLRequest* request) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (java_object_.is_null())
    return std::unique_ptr<XWalkWebResourceResponse>();
  content::ResourceRequestInfo* info = content::ResourceRequestInfo::ForRequest(request);
  bool is_main_frame = info && info->GetResourceType() == content::ResourceType::kMainFrame;
  bool has_user_gesture = info && info->HasUserGesture();

  vector<string> headers_names;
  vector<string> headers_values;
  {
    net::HttpRequestHeaders headers;
    if (!request->GetFullRequestHeaders(&headers))
      headers = request->extra_request_headers();
    net::HttpRequestHeaders::Iterator headers_iterator(headers);
    while (headers_iterator.GetNext()) {
      headers_names.push_back(headers_iterator.name());
      headers_values.push_back(headers_iterator.value());
    }
  }

  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jstring> jstring_url =
      ConvertUTF8ToJavaString(env, location.spec());
  ScopedJavaLocalRef<jstring> jstring_method =
      ConvertUTF8ToJavaString(env, request->method());
  ScopedJavaLocalRef<jobjectArray> jstringArray_headers_names =
      ToJavaArrayOfStrings(env, headers_names);
  ScopedJavaLocalRef<jobjectArray> jstringArray_headers_values =
      ToJavaArrayOfStrings(env, headers_values);

  ScopedJavaLocalRef<jobject> ret =
      Java_XWalkContentsIoThreadClient_shouldInterceptRequest(
          env,
          java_object_,
          jstring_url,
          is_main_frame,
          has_user_gesture,
          jstring_method,
          jstringArray_headers_names,
          jstringArray_headers_values);
  if (ret.is_null())
    return std::unique_ptr<XWalkWebResourceResponse>();
  return std::unique_ptr<XWalkWebResourceResponse>(
      new XWalkWebResourceResponseImpl(ret));
}

bool XWalkContentsIoThreadClient::ShouldBlockContentUrls() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (java_object_.is_null())
    return false;

  JNIEnv* env = AttachCurrentThread();
  return Java_XWalkContentsIoThreadClient_shouldBlockContentUrls(
      env, java_object_);
}

bool XWalkContentsIoThreadClient::ShouldBlockFileUrls() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (java_object_.is_null())
    return false;

  JNIEnv* env = AttachCurrentThread();
  return Java_XWalkContentsIoThreadClient_shouldBlockFileUrls(
      env, java_object_);
}

bool XWalkContentsIoThreadClient::ShouldBlockNetworkLoads() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (java_object_.is_null())
    return false;

  JNIEnv* env = AttachCurrentThread();
  return Java_XWalkContentsIoThreadClient_shouldBlockNetworkLoads(
      env, java_object_);
}

bool XWalkContentsIoThreadClient::ShouldAcceptThirdPartyCookies() const {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (java_object_.is_null())
    return false;

  JNIEnv* env = AttachCurrentThread();
  return Java_XWalkContentsIoThreadClient_shouldAcceptThirdPartyCookies(env, java_object_);
}

void XWalkContentsIoThreadClient::OnReceivedResponseHeaders(
    const net::URLRequest* request,
    const net::HttpResponseHeaders* response_headers) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (java_object_.is_null())
    return;

  JNIEnv* env = AttachCurrentThread();
  WebResourceRequest web_request(env, request);

  vector<string> response_header_names;
  vector<string> response_header_values;
  {
    size_t headers_iterator = 0;
    string header_name, header_value;
    while (response_headers->EnumerateHeaderLines(
        &headers_iterator, &header_name, &header_value)) {
      response_header_names.push_back(header_name);
      response_header_values.push_back(header_value);
    }
  }

  string mime_type, encoding;
  response_headers->GetMimeTypeAndCharset(&mime_type, &encoding);
  ScopedJavaLocalRef<jstring> jstring_mime_type =
      ConvertUTF8ToJavaString(env, mime_type);
  ScopedJavaLocalRef<jstring> jstring_encoding =
      ConvertUTF8ToJavaString(env, encoding);
  int status_code = response_headers->response_code();
  ScopedJavaLocalRef<jstring> jstring_reason =
      ConvertUTF8ToJavaString(env, response_headers->GetStatusText());
  ScopedJavaLocalRef<jobjectArray> jstringArray_response_header_names =
      ToJavaArrayOfStrings(env, response_header_names);
  ScopedJavaLocalRef<jobjectArray> jstringArray_response_header_values =
      ToJavaArrayOfStrings(env, response_header_values);

  Java_XWalkContentsIoThreadClient_onReceivedResponseHeaders(
      env,
      java_object_,
      web_request.jstring_url,
      web_request.is_main_frame,
      web_request.has_user_gesture,
      web_request.jstring_method,
      web_request.jstringArray_header_names,
      web_request.jstringArray_header_values,
      jstring_mime_type,
      jstring_encoding,
      status_code,
      jstring_reason,
      jstringArray_response_header_names,
      jstringArray_response_header_values);
}

}  // namespace xwalk
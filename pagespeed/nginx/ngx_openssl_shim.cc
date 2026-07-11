/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "ngx_openssl_shim.h"

#if (NGX_SSL)

#include <dlfcn.h>

namespace net_instaweb {

// ABI-stable OpenSSL SSL_ctrl constants, spelled out: the headers this
// translation unit sees are BoringSSL's, which poison the SSL_ctrl macros
// (BoringSSL has no SSL_ctrl). The call targets nginx's OpenSSL.
const int kSslCtrlSetTlsextHostname = 55;  // SSL_CTRL_SET_TLSEXT_HOSTNAME
const long kTlsextNametypeHostName = 0;    // TLSEXT_NAMETYPE_host_name

namespace {

template <typename T>
void ResolveInto(const char* name, T* fn) {
  *fn = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

}  // namespace

NgxOpenSslShim* NgxOpenSslShim::Get() {
  static NgxOpenSslShim shim;
  return &shim;
}

NgxOpenSslShim::NgxOpenSslShim() {
  ResolveInto("SSL_CTX_load_verify_locations", &load_verify_locations_);
  ResolveInto("SSL_CTX_set_default_verify_paths", &set_default_verify_paths_);
  ResolveInto("SSL_get_verify_result", &get_verify_result_);
  // OpenSSL 3.x renamed the symbol (SSL_get_peer_certificate became a macro
  // for SSL_get1_peer_certificate); both return an owned reference.
  ResolveInto("SSL_get1_peer_certificate", &get_peer_certificate_);
  if (get_peer_certificate_ == nullptr) {
    ResolveInto("SSL_get_peer_certificate", &get_peer_certificate_);
  }
  ResolveInto("X509_free", &x509_free_);
  ResolveInto("X509_check_ip_asc", &x509_check_ip_asc_);
  ResolveInto("X509_verify_cert_error_string", &verify_cert_error_string_);
  ResolveInto("SSL_ctrl", &ssl_ctrl_);

  // verify_cert_error_string_ only affects log quality and is optional.
  ok_ = load_verify_locations_ != nullptr &&
        set_default_verify_paths_ != nullptr && get_verify_result_ != nullptr &&
        get_peer_certificate_ != nullptr && x509_free_ != nullptr &&
        x509_check_ip_asc_ != nullptr && ssl_ctrl_ != nullptr;
}

const char* NgxOpenSslShim::X509VerifyCertErrorString(long code) {
  if (verify_cert_error_string_ == nullptr) {
    return "unknown";
  }
  return verify_cert_error_string_(code);
}

int NgxOpenSslShim::SslSetTlsextHostName(SSL* ssl, const char* name) {
  return static_cast<int>(ssl_ctrl_(ssl, kSslCtrlSetTlsextHostname,
                                    kTlsextNametypeHostName,
                                    const_cast<char*>(name)));
}

}  // namespace net_instaweb

#endif  // NGX_SSL

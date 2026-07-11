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

//
// The pagespeed module statically links its own TLS library (BoringSSL, for
// curl and internal crypto), with those symbols hidden via
// ngx_pagespeed.lds. The native fetcher's TLS support operates on SSL
// objects created through NGINX's ngx_ssl_* API, and those belong to
// whatever TLS library nginx itself links (typically the system OpenSSL).
// A direct SSL_*/X509_* call from module code would bind to the module's
// statically linked definitions at link time and mix two TLS libraries'
// ABIs on one object. This shim resolves the few entry points we need with
// dlsym(RTLD_DEFAULT) instead: the module's own definitions are local (not
// in its dynamic symbol table), so the lookup binds to the same library
// nginx uses.

#ifndef NET_INSTAWEB_NGX_OPENSSL_SHIM_H_
#define NET_INSTAWEB_NGX_OPENSSL_SHIM_H_

extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#if (NGX_SSL)
#include <ngx_event.h>
#endif
}

#if (NGX_SSL)

namespace net_instaweb {

class NgxOpenSslShim {
 public:
  // Returns the process-wide instance; resolution happens on first call.
  // Not thread-safe on first call — call during single-threaded startup.
  static NgxOpenSslShim* Get();

  // False when a required entry point could not be resolved. Https fetching
  // must stay disabled in that case.
  bool ok() const { return ok_; }

  int SslCtxLoadVerifyLocations(SSL_CTX* ctx, const char* file,
                                const char* dir) {
    return load_verify_locations_(ctx, file, dir);
  }
  int SslCtxSetDefaultVerifyPaths(SSL_CTX* ctx) {
    return set_default_verify_paths_(ctx);
  }
  long SslGetVerifyResult(const SSL* ssl) { return get_verify_result_(ssl); }
  // Returns an owned reference; release with X509Free().
  X509* SslGetPeerCertificate(const SSL* ssl) {
    return get_peer_certificate_(ssl);
  }
  void X509Free(X509* cert) { x509_free_(cert); }
  // Hostname check for IP-literal hosts (X509_check_ip_asc): nginx's
  // ngx_ssl_check_host only matches DNS names, never IP SANs. Returns 1 on
  // match, like the OpenSSL original.
  int X509CheckIpAsc(X509* cert, const char* ip) {
    return x509_check_ip_asc_(cert, ip, 0);
  }
  // Human-readable verification failure; "unknown" when the symbol is
  // unavailable.
  const char* X509VerifyCertErrorString(long code);
  // SSL_set_tlsext_host_name equivalent (the OpenSSL macro over SSL_ctrl).
  int SslSetTlsextHostName(SSL* ssl, const char* name);

 private:
  NgxOpenSslShim();

  int (*load_verify_locations_)(SSL_CTX*, const char*, const char*);
  int (*set_default_verify_paths_)(SSL_CTX*);
  long (*get_verify_result_)(const SSL*);
  X509* (*get_peer_certificate_)(const SSL*);
  void (*x509_free_)(X509*);
  int (*x509_check_ip_asc_)(X509*, const char*, unsigned int);
  const char* (*verify_cert_error_string_)(long);
  long (*ssl_ctrl_)(SSL*, int, long, void*);
  bool ok_;

  NgxOpenSslShim(const NgxOpenSslShim&) = delete;
  NgxOpenSslShim& operator=(const NgxOpenSslShim&) = delete;
};

}  // namespace net_instaweb

#endif  // NGX_SSL

#endif  // NET_INSTAWEB_NGX_OPENSSL_SHIM_H_

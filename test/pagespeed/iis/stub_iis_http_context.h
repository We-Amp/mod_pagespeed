// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Do-nothing implementations of the IIS request interfaces (httpserv.h).
// Every method returns a zero value; a test derives from the stub it needs
// and overrides only the calls the code under test makes.

#ifndef TEST_PAGESPEED_IIS_STUB_IIS_HTTP_CONTEXT_H_
#define TEST_PAGESPEED_IIS_STUB_IIS_HTTP_CONTEXT_H_

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <sal.h>
#include <httpserv.h>

namespace net_instaweb {

class StubHttpRequest : public IHttpRequest {
 public:
  HTTP_REQUEST * GetRawHttpRequest() override { return {}; }
  const HTTP_REQUEST * GetRawHttpRequest() const override { return {}; }
  PCSTR GetHeader(PCSTR pszHeaderName, USHORT * pcchHeaderValue) const override { return {}; }
  PCSTR GetHeader(HTTP_HEADER_ID ulHeaderIndex, USHORT * pcchHeaderValue) const override { return {}; }
  HRESULT SetHeader(PCSTR pszHeaderName, PCSTR pszHeaderValue, USHORT cchHeaderValue, BOOL fReplace) override { return {}; }
  HRESULT SetHeader(HTTP_HEADER_ID ulHeaderIndex, PCSTR pszHeaderValue, USHORT cchHeaderValue, BOOL fReplace) override { return {}; }
  HRESULT DeleteHeader(PCSTR pszHeaderName) override { return {}; }
  HRESULT DeleteHeader(HTTP_HEADER_ID ulHeaderIndex) override { return {}; }
  PCSTR GetHttpMethod() const override { return {}; }
  HRESULT SetHttpMethod(PCSTR pszHttpMethod) override { return {}; }
  HRESULT SetUrl(PCWSTR pszUrl, DWORD cchUrl, BOOL fResetQueryString) override { return {}; }
  HRESULT SetUrl(PCSTR pszUrl, DWORD cchUrl, BOOL fResetQueryString) override { return {}; }
  BOOL GetUrlChanged() const override { return {}; }
  PCWSTR GetForwardedUrl() const override { return {}; }
  PSOCKADDR GetLocalAddress() const override { return {}; }
  PSOCKADDR GetRemoteAddress() const override { return {}; }
  HRESULT ReadEntityBody(VOID * pvBuffer, DWORD cbBuffer, BOOL fAsync, DWORD * pcbBytesReceived, BOOL * pfCompletionPending) override { return {}; }
  HRESULT InsertEntityBody(VOID * pvBuffer, DWORD cbBuffer) override { return {}; }
  DWORD GetRemainingEntityBytes() override { return {}; }
  VOID GetHttpVersion(USHORT * pMajorVersion, USHORT * pMinorVersion) const override {}
  HRESULT GetClientCertificate(HTTP_SSL_CLIENT_CERT_INFO ** ppClientCertInfo, BOOL * pfClientCertNegotiated) override { return {}; }
  HRESULT NegotiateClientCertificate(BOOL fAsync, BOOL * pfCompletionPending) override { return {}; }
  DWORD GetSiteId() const override { return {}; }
  HRESULT GetHeaderChanges(DWORD dwOldChangeNumber, DWORD * pdwNewChangeNumber, PCSTR knownHeaderSnapshot[HttpHeaderRequestMaximum], DWORD * pdwUnknownHeaderSnapshot, PCSTR ** ppUnknownHeaderNameSnapshot, PCSTR ** ppUnknownHeaderValueSnapshot, DWORD diffedKnownHeaderIndices[HttpHeaderRequestMaximum+1], DWORD * pdwDiffedUnknownHeaders, DWORD ** ppDiffedUnknownHeaderIndices) override { return {}; }
};

class StubHttpResponse : public IHttpResponse {
 public:
  HTTP_RESPONSE * GetRawHttpResponse() override { return {}; }
  const HTTP_RESPONSE * GetRawHttpResponse() const override { return {}; }
  IHttpCachePolicy * GetCachePolicy() override { return {}; }
  HRESULT SetStatus(USHORT statusCode, PCSTR pszReason, USHORT uSubStatus, HRESULT hrErrorToReport, IAppHostConfigException * pException, BOOL fTrySkipCustomErrors) override { return {}; }
  HRESULT SetHeader(PCSTR pszHeaderName, PCSTR pszHeaderValue, USHORT cchHeaderValue, BOOL fReplace) override { return {}; }
  HRESULT SetHeader(HTTP_HEADER_ID ulHeaderIndex, PCSTR pszHeaderValue, USHORT cchHeaderValue, BOOL fReplace) override { return {}; }
  HRESULT DeleteHeader(PCSTR pszHeaderName) override { return {}; }
  HRESULT DeleteHeader(HTTP_HEADER_ID ulHeaderIndex) override { return {}; }
  PCSTR GetHeader(PCSTR pszHeaderName, USHORT * pcchHeaderValue) const override { return {}; }
  PCSTR GetHeader(HTTP_HEADER_ID ulHeaderIndex, USHORT * pcchHeaderValue) const override { return {}; }
  VOID Clear() override {}
  VOID ClearHeaders() override {}
  VOID SetNeedDisconnect() override {}
  VOID ResetConnection() override {}
  VOID DisableKernelCache(ULONG reason) override {}
  BOOL GetKernelCacheEnabled() const override { return {}; }
  VOID SuppressHeaders() override {}
  BOOL GetHeadersSuppressed() const override { return {}; }
  HRESULT Flush(BOOL fAsync, BOOL fMoreData, DWORD * pcbSent, BOOL * pfCompletionExpected) override { return {}; }
  HRESULT Redirect(PCSTR pszUrl, BOOL fResetStatusCode, BOOL fIncludeParameters) override { return {}; }
  HRESULT WriteEntityChunkByReference(HTTP_DATA_CHUNK * pDataChunk, LONG lInsertPosition) override { return {}; }
  HRESULT WriteEntityChunks(HTTP_DATA_CHUNK * pDataChunks, DWORD nChunks, BOOL fAsync, BOOL fMoreData, DWORD * pcbSent, BOOL * pfCompletionExpected) override { return {}; }
  VOID DisableBuffering() override {}
  VOID GetStatus(USHORT * pStatusCode, USHORT * pSubStatus, PCSTR * ppszReason, USHORT * pcchReason, HRESULT * phrErrorToReport, PCWSTR * ppszModule, DWORD * pdwNotification, IAppHostConfigException ** ppException, BOOL * pfTrySkipCustomErrors) override {}
  HRESULT SetErrorDescription(PCWSTR pszDescription, DWORD cchDescription, BOOL fHtmlEncode) override { return {}; }
  PCWSTR GetErrorDescription(DWORD * pcchDescription) override { return {}; }
  HRESULT GetHeaderChanges(DWORD dwOldChangeNumber, DWORD * pdwNewChangeNumber, PCSTR knownHeaderSnapshot[HttpHeaderResponseMaximum], DWORD * pdwUnknownHeaderSnapshot, PCSTR ** ppUnknownHeaderNameSnapshot, PCSTR ** ppUnknownHeaderValueSnapshot, DWORD diffedKnownHeaderIndices[HttpHeaderResponseMaximum+1], DWORD * pdwDiffedUnknownHeaders, DWORD ** ppDiffedUnknownHeaderIndices) override { return {}; }
  VOID CloseConnection() override {}
};

class StubHttpContext : public IHttpContext {
 public:
  IHttpSite * GetSite() override { return {}; }
  IHttpApplication * GetApplication() override { return {}; }
  IHttpConnection * GetConnection() override { return {}; }
  IHttpRequest * GetRequest() override { return {}; }
  IHttpResponse * GetResponse() override { return {}; }
  BOOL GetResponseHeadersSent() const override { return {}; }
  IHttpUser * GetUser() const override { return {}; }
  IHttpModuleContextContainer * GetModuleContextContainer() override { return {}; }
  VOID IndicateCompletion(REQUEST_NOTIFICATION_STATUS notificationStatus) override {}
  HRESULT PostCompletion(DWORD cbBytes) override { return {}; }
  VOID DisableNotifications(DWORD dwNotifications, DWORD dwPostNotifications) override {}
  BOOL GetNextNotification(REQUEST_NOTIFICATION_STATUS status, DWORD * pdwNotification, BOOL * pfIsPostNotification, CHttpModule ** ppModuleInfo, IHttpEventProvider ** ppRequestOutput) override { return {}; }
  BOOL GetIsLastNotification(REQUEST_NOTIFICATION_STATUS status) override { return {}; }
  HRESULT ExecuteRequest(BOOL fAsync, IHttpContext * pHttpContext, DWORD dwExecuteFlags, IHttpUser * pHttpUser, BOOL * pfCompletionExpected) override { return {}; }
  DWORD GetExecuteFlags() const override { return {}; }
  HRESULT GetServerVariable(PCSTR pszVariableName, PCWSTR * ppszValue, DWORD * pcchValueLength) override { return {}; }
  HRESULT GetServerVariable(PCSTR pszVariableName, PCSTR * ppszValue, DWORD * pcchValueLength) override { return {}; }
  HRESULT SetServerVariable(PCSTR pszVariableName, PCWSTR pszVariableValue) override { return {}; }
  VOID * AllocateRequestMemory(DWORD cbAllocation) override { return {}; }
  IHttpUrlInfo * GetUrlInfo() override { return {}; }
  IMetadataInfo * GetMetadata() override { return {}; }
  PCWSTR GetPhysicalPath(DWORD * pcchPhysicalPath) override { return {}; }
  PCWSTR GetScriptName(DWORD * pcchScriptName) const override { return {}; }
  PCWSTR GetScriptTranslated(DWORD * pcchScriptTranslated) override { return {}; }
  IScriptMapInfo * GetScriptMap() const override { return {}; }
  VOID SetRequestHandled() override {}
  IHttpFileInfo * GetFileInfo() const override { return {}; }
  HRESULT MapPath(PCWSTR pszUrl, PWSTR pszPhysicalPath, DWORD * pcbPhysicalPath) override { return {}; }
  HRESULT NotifyCustomNotification(ICustomNotificationProvider * pCustomOutput, BOOL * pfCompletionExpected) override { return {}; }
  IHttpContext * GetParentContext() const override { return {}; }
  IHttpContext * GetRootContext() const override { return {}; }
  HRESULT CloneContext(DWORD dwCloneFlags, IHttpContext ** ppHttpContext) override { return {}; }
  HRESULT ReleaseClonedContext() override { return {}; }
  HRESULT GetCurrentExecutionStats(DWORD * pdwNotification, DWORD * pdwNotificationStartTickCount, PCWSTR * ppszModule, DWORD * pdwModuleStartTickCount, DWORD * pdwAsyncNotification, DWORD * pdwAsyncNotificationStartTickCount) const override { return {}; }
  IHttpTraceContext * GetTraceContext() const override { return {}; }
  HRESULT GetServerVarChanges(DWORD dwOldChangeNumber, DWORD * pdwNewChangeNumber, DWORD * pdwVariableSnapshot, PCSTR ** ppVariableNameSnapshot, PCWSTR ** ppVariableValueSnapshot, DWORD * pdwDiffedVariables, DWORD ** ppDiffedVariableIndices) override { return {}; }
  HRESULT CancelIo() override { return {}; }
  HRESULT MapHandler(DWORD dwSiteId, PCWSTR pszSiteName, PCWSTR pszUrl, PCSTR pszVerb, IScriptMapInfo ** ppScriptMap, BOOL fIgnoreWildcardMappings) override { return {}; }
  HRESULT GetExtendedInterface(HTTP_CONTEXT_INTERFACE_VERSION version, PVOID * ppInterface) override { return {}; }
};

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_IIS_STUB_IIS_HTTP_CONTEXT_H_

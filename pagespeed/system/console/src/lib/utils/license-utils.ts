/**
 * Decode the payload of a JWT-style license token.
 * Returns null if the token is malformed.
 */
export function decodeLicensePayload(token: string): object | null {
  try {
    const parts = token.split(".");
    if (parts.length < 2) return null;
    const payload = parts[1];
    const json = atob(payload.replace(/-/g, "+").replace(/_/g, "/"));
    return JSON.parse(json);
  } catch {
    return null;
  }
}

/** Map license error codes to human-readable messages. */
export function getLicenseErrorMessage(error: string): string {
  const messages: Record<string, string> = {
    expired: "Your license has expired. Please renew to continue.",
    invalid_key: "The license key is invalid. Please check and try again.",
    domain_mismatch:
      "This license is not valid for the current domain.",
    revoked: "This license has been revoked. Please contact support.",
    activation_limit:
      "The maximum number of activations has been reached.",
    network_error:
      "Could not reach the license server. Please check your connection.",
    server_error:
      "The license server encountered an error. Please try again later.",
  };

  return messages[error] ?? `License error: ${error}`;
}

/** Format a Unix epoch (seconds) as a human-readable date string. */
export function formatLicenseDate(epoch: number): string {
  if (!epoch || epoch <= 0) return "N/A";
  const date = new Date(epoch * 1000);
  return date.toLocaleDateString("en-US", {
    year: "numeric",
    month: "long",
    day: "numeric",
  });
}

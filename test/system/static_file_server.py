#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""
Simple static file server with directory index support.

This server extends Python's http.server to:
1. Serve index.html for directory requests (if index.html exists)
2. Set proper Content-Type headers for common file types
3. Support command-line configuration of port and directory

Usage:
    python3 static_file_server.py PORT DIRECTORY

Example:
    python3 static_file_server.py 8081 /var/www/html
"""

import http.server
import mimetypes
import os
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


class StaticFileHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP request handler that serves index.html for directory requests."""

    # Extended MIME type mappings for common web files
    CONTENT_TYPES = {
        # Web
        '.html': 'text/html; charset=utf-8',
        '.htm': 'text/html; charset=utf-8',
        '.css': 'text/css; charset=utf-8',
        '.js': 'application/javascript; charset=utf-8',
        '.mjs': 'application/javascript; charset=utf-8',
        '.json': 'application/json; charset=utf-8',
        '.xml': 'application/xml; charset=utf-8',
        '.txt': 'text/plain; charset=utf-8',
        # Images
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.gif': 'image/gif',
        '.webp': 'image/webp',
        '.svg': 'image/svg+xml',
        '.ico': 'image/x-icon',
        '.avif': 'image/avif',
        # Fonts
        '.woff': 'font/woff',
        '.woff2': 'font/woff2',
        '.ttf': 'font/ttf',
        '.otf': 'font/otf',
        '.eot': 'application/vnd.ms-fontobject',
        # Media
        '.mp4': 'video/mp4',
        '.webm': 'video/webm',
        '.mp3': 'audio/mpeg',
        '.ogg': 'audio/ogg',
        '.wav': 'audio/wav',
        # Documents
        '.pdf': 'application/pdf',
        # Archives
        '.zip': 'application/zip',
        '.gz': 'application/gzip',
        '.tar': 'application/x-tar',
    }

    def translate_path(self, path):
        """Translate URL path to filesystem path, handling directory index."""
        # Parse and unquote the path
        path = urlparse(path).path
        path = unquote(path)

        # Remove leading slash and normalize
        if path.startswith('/'):
            path = path[1:]

        # Build the full filesystem path
        fs_path = Path(self.directory) / path

        # If it's a directory, check for index.html
        if fs_path.is_dir():
            index_path = fs_path / 'index.html'
            if index_path.is_file():
                return str(index_path)

        return str(fs_path)

    def guess_type(self, path):
        """Guess the Content-Type based on file extension."""
        ext = Path(path).suffix.lower()

        # Check our extended mapping first
        if ext in self.CONTENT_TYPES:
            return self.CONTENT_TYPES[ext]

        # Fall back to mimetypes module
        mime_type, _ = mimetypes.guess_type(path)
        if mime_type:
            return mime_type

        # Default to binary
        return 'application/octet-stream'

    def _get_cache_headers(self, path):
        """Return cache headers matching Apache's debug.conf.template config.

        Apache does NOT set Cache-Control for most static files by default.
        Only specific directories get explicit Cache-Control headers.
        Resources without explicit Cache-Control but with Last-Modified are
        considered implicitly cacheable by PSOL (using implicit_cache_ttl_ms,
        default 300s).
        """
        headers = {}
        # Last-Modified for all files (Apache sends this by default)
        stat = os.stat(path)
        import email.utils
        last_modified = email.utils.formatdate(stat.st_mtime, usegmt=True)
        headers['Last-Modified'] = last_modified

        # Match Apache's per-directory Cache-Control settings from
        # install/debug.conf.template
        if '/no_cache/' in self.path:
            headers['Cache-Control'] = 'no-cache'
        # No default Cache-Control for other paths — matches Apache behavior.
        # PSOL will use implicit_cache_ttl_ms (300s) for IPRO resources.

        return headers

    def do_GET(self):
        """Handle GET requests."""
        # Translate the path
        path = self.translate_path(self.path)

        # Check if file exists
        if not os.path.exists(path):
            self.send_error(404, f"File not found: {self.path}")
            return

        # Check if it's a directory without index.html
        if os.path.isdir(path):
            # No index.html found, send 403 or directory listing
            self.send_error(403, f"Directory listing not allowed: {self.path}")
            return

        # Serve the file
        try:
            with open(path, 'rb') as f:
                content = f.read()

            self.send_response(200)
            self.send_header('Content-Type', self.guess_type(path))
            self.send_header('Content-Length', len(content))
            # Add cache headers matching Apache's configuration
            for name, value in self._get_cache_headers(path).items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(content)

        except IOError as e:
            self.send_error(500, f"Error reading file: {e}")

    def do_HEAD(self):
        """Handle HEAD requests."""
        path = self.translate_path(self.path)

        if not os.path.exists(path):
            self.send_error(404, f"File not found: {self.path}")
            return

        if os.path.isdir(path):
            self.send_error(403, f"Directory listing not allowed: {self.path}")
            return

        try:
            stat = os.stat(path)
            self.send_response(200)
            self.send_header('Content-Type', self.guess_type(path))
            self.send_header('Content-Length', stat.st_size)
            # Match GET behavior for cache headers
            for name, value in self._get_cache_headers(path).items():
                self.send_header(name, value)
            self.end_headers()

        except IOError as e:
            self.send_error(500, f"Error reading file: {e}")

    def log_message(self, format, *args):
        """Log HTTP requests to stderr."""
        sys.stderr.write("%s - - [%s] %s\n" %
                         (self.address_string(),
                          self.log_date_time_string(),
                          format % args))


def run_server(port, directory, bind_address='127.0.0.1'):
    """Run the static file server.

    Args:
        port: Port number to listen on
        directory: Root directory to serve files from
        bind_address: Address to bind to (default: 127.0.0.1)
    """
    # Validate directory
    if not os.path.isdir(directory):
        print(f"Error: Directory does not exist: {directory}", file=sys.stderr)
        sys.exit(1)

    # Create a handler class with the directory bound
    # Note: SimpleHTTPRequestHandler.__init__ overrides class attributes with
    # self.directory = os.getcwd() if directory=None, so we must use functools.partial
    # to pass the directory to __init__ properly.
    from functools import partial
    abs_directory = os.path.abspath(directory)
    handler = partial(StaticFileHandler, directory=abs_directory)

    # Create and start the server
    server_address = (bind_address, port)
    httpd = http.server.HTTPServer(server_address, handler)

    print(f"Serving files from {abs_directory}", file=sys.stderr)
    print(f"Server listening on {bind_address}:{port}", file=sys.stderr)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...", file=sys.stderr)
        httpd.shutdown()


def main():
    """Main entry point."""
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} PORT DIRECTORY [BIND_ADDRESS]", file=sys.stderr)
        print(f"Example: {sys.argv[0]} 8081 /var/www/html", file=sys.stderr)
        print(f"         {sys.argv[0]} 8081 /var/www/html 0.0.0.0", file=sys.stderr)
        sys.exit(1)

    try:
        port = int(sys.argv[1])
        if port < 1 or port > 65535:
            raise ValueError("Port must be between 1 and 65535")
    except ValueError as e:
        print(f"Error: Invalid port number: {sys.argv[1]} ({e})", file=sys.stderr)
        sys.exit(1)

    directory = sys.argv[2]

    bind_address = '127.0.0.1'
    if len(sys.argv) >= 4:
        bind_address = sys.argv[3]

    run_server(port, directory, bind_address)


if __name__ == '__main__':
    main()

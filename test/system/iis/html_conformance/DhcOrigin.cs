// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The origin of the IIS HTML cache conformance site (App_Code): every path
// is answered with the row's status, header lines and body, which the
// provider (daemon_html_provider_iis.py) writes to the state directory, and
// the header lines of every request are logged there. Test equipment only.
//
// State files (C:\dhc\state):
//   origin.txt      line 1: the status; then one "name<TAB>value" per line
//   origin.body     the body, bytes
//   requests.log    appended: one block per request, "name<TAB>value"
//                   lines, then a line holding only "--"
//   hold-next       when present, the next request deletes it and waits
//                   (headers not sent) while "held" exists
//   held            see hold-next
// /__dhc_pid answers the worker process id (text/plain), and logs nothing.

using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Web;

public class DhcOrigin : IHttpHandler
{
    const string State = @"C:\dhc\state";
    static readonly object Lock = new object();

    public bool IsReusable { get { return true; } }

    static string Clean(string s)
    {
        return s.Replace("\t", " ").Replace("\r", " ").Replace("\n", " ");
    }

    public void ProcessRequest(HttpContext ctx)
    {
        HttpResponse r = ctx.Response;
        r.SuppressDefaultCacheControlHeader = true;
        if (ctx.Request.RawUrl == "/__dhc_pid")
        {
            r.ContentType = "text/plain";
            r.Charset = "";
            r.Write(Process.GetCurrentProcess().Id.ToString());
            return;
        }
        var log = new StringBuilder();
        foreach (string name in ctx.Request.Headers.AllKeys)
        {
            foreach (string value in ctx.Request.Headers.GetValues(name))
            {
                log.Append(Clean(name)).Append('\t').Append(Clean(value)).Append('\n');
            }
        }
        log.Append("--\n");
        bool hold = false;
        lock (Lock)
        {
            File.AppendAllText(Path.Combine(State, "requests.log"), log.ToString());
            string next = Path.Combine(State, "hold-next");
            if (File.Exists(next))
            {
                File.Delete(next);
                hold = true;
            }
        }
        if (hold)
        {
            string held = Path.Combine(State, "held");
            DateTime until = DateTime.UtcNow.AddSeconds(120);
            while (File.Exists(held) && DateTime.UtcNow < until) Thread.Sleep(20);
        }

        string[] spec = File.ReadAllLines(Path.Combine(State, "origin.txt"));
        byte[] body = File.ReadAllBytes(Path.Combine(State, "origin.body"));
        int status = int.Parse(spec[0]);
        string range = ctx.Request.Headers["Range"] ?? "";
        string contentRange = null;
        if (status == 200 && range.StartsWith("bytes=") && range.IndexOf(',') < 0)
        {
            string[] ab = range.Substring(6).Split('-');
            int first, last;
            if (ab.Length == 2 && int.TryParse(ab[0], out first) && first < body.Length)
            {
                if (!int.TryParse(ab[1], out last) || last >= body.Length) last = body.Length - 1;
                contentRange = "bytes " + first + "-" + last + "/" + body.Length;
                byte[] part = new byte[last - first + 1];
                Array.Copy(body, first, part, 0, part.Length);
                body = part;
                status = 206;
            }
        }
        r.StatusCode = status;
        r.Charset = "";
        bool typed = false;
        for (int i = 1; i < spec.Length; ++i)
        {
            int tab = spec[i].IndexOf('\t');
            if (tab <= 0) continue;
            string name = spec[i].Substring(0, tab);
            string value = spec[i].Substring(tab + 1);
            if (string.Equals(name, "Content-Type", StringComparison.OrdinalIgnoreCase))
            {
                r.ContentType = value;
                typed = true;
                continue;
            }
            r.AppendHeader(name, value);
        }
        if (!typed) r.ContentType = "application/octet-stream";
        if (contentRange != null) r.AppendHeader("Content-Range", contentRange);
        r.BinaryWrite(body);
    }
}

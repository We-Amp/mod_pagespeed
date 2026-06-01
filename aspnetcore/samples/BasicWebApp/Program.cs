using WeAmp.PageSpeed.AspNetCore.DependencyInjection;

var builder = WebApplication.CreateBuilder(args);

// Add PageSpeed middleware - reads configuration from appsettings.json
builder.Services.AddPageSpeed(builder.Configuration);

var app = builder.Build();

// Optional: Use PageSpeed middleware (currently a no-op, reserved for future features)
app.UsePageSpeed();

// Map health check endpoints
app.MapHealthChecks("/health");
app.MapPageSpeedHealthCheck("/health/pagespeed");
app.MapPageSpeedInfo("/pagespeed/info");

// Sample endpoints
app.MapGet("/", () => Results.Content("""
    <!DOCTYPE html>
    <html>
    <head>
        <title>PageSpeed Test Page</title>
        <style>
            body { font-family: Arial, sans-serif; margin: 40px; }
            h1 { color: #333; }
            .info { background: #f0f0f0; padding: 20px; border-radius: 8px; }
            pre { background: #272822; color: #f8f8f2; padding: 15px; border-radius: 4px; overflow-x: auto; }
        </style>
    </head>
    <body>
        <h1>PageSpeed ASP.NET Core Middleware Test</h1>
        <div class="info">
            <p>This page is being optimized by PageSpeed via Envoy sidecar.</p>
            <p>Check the following endpoints:</p>
            <ul>
                <li><a href="/health">/health</a> - Overall health check</li>
                <li><a href="/health/pagespeed">/health/pagespeed</a> - PageSpeed-specific health</li>
                <li><a href="/pagespeed/info">/pagespeed/info</a> - Sidecar status and admin info</li>
            </ul>
        </div>
        <h2>How It Works</h2>
        <p>The PageSpeed sidecar runs as a separate process:</p>
        <pre>
    Client Request (port 8080)
           ↓
    Envoy + PageSpeed Filter
           ↓
    Optimized Request → Kestrel (port 5000)
           ↓
    Response flows back through PageSpeed
           ↓
    Optimized HTML/CSS/JS/Images → Client
        </pre>
        <p>Look for <code>.pagespeed.</code> in resource URLs to verify optimization is active.</p>
    </body>
    </html>
    """, "text/html"));

app.MapGet("/api/data", () => new { message = "API endpoints bypass PageSpeed", timestamp = DateTime.UtcNow });

app.Run();

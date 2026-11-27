#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Build the operator population and probe targets.

Inputs: ai.robots.txt robots.json, Cloudflare Radar bots directory details.
Output: targets.csv (operator, sources, host, basis) and excluded.csv.
"""
import csv, json, sys
from urllib.parse import urlparse

robots = json.load(open(sys.argv[1]))
radar = json.load(open(sys.argv[2]))
out = sys.argv[3]
WK = "https://%s/.well-known/http-message-signatures-directory"
AI_CATS = {"AI_CRAWLER", "AI_ASSISTANT", "AI_SEARCH"}

# ai.robots.txt user-agent -> canonical operator (None = excluded, reason)
UA = {
 "AddSearchBot": "AddSearch", "AgentDataBot": None, "AgentTimes": "The Agent Times",
 "AI2Bot": "Ai2", "AI2Bot-DeepResearchEval": "Ai2", "Ai2Bot-Dolma": "Ai2", "aiHitBot": "aiHit",
 "AIWebIndex": "Lyrenth", "amazon-kendra": "Amazon", "amazon-QBusiness": "Amazon", "Amazonbot": "Amazon",
 "AmazonBuyForMe": "Amazon", "Amzn-SearchBot": "Amazon", "Amzn-User": "Amazon", "Andibot": "Andi",
 "Anomura": "Direqt", "anthropic-ai": "Anthropic", "ApifyBot": "Apify", "ApifyWebsiteContentCrawler": "Apify",
 "Applebot": "Apple", "Applebot-Extended": "Apple", "Aranet-SearchBot": None, "atlassian-bot": "Atlassian",
 "Awario": "Awario", "AzureAI-SearchBot": "Microsoft", "bedrockbot": "Amazon", "bigsur.ai": "Big Sur AI",
 "BixelBot": "Bixel", "Bravebot": "Brave", "Brightbot": "Bright Data", "Brightbot 1.0": "Bright Data",
 "BuddyBot": "BuddyBotLearning", "Bytespider": "ByteDance", "CCBot": "Common Crawl", "Channel3Bot": "Channel3",
 "ChatGLM-Spider": "Zhipu AI", "ChatGPT Agent": "OpenAI", "ChatGPT-User": "OpenAI", "Claude-Code": "Anthropic",
 "Claude-SearchBot": "Anthropic", "Claude-User": "Anthropic", "Claude-Web": "Anthropic", "ClaudeBot": "Anthropic",
 "Cloudflare-AutoRAG": "Cloudflare", "CloudflareBrowserRenderingCrawler": "Cloudflare", "CloudVertexBot": "Google",
 "Code": "GitHub", "cohere-ai": "Cohere", "cohere-training-data-crawler": "Cohere", "Cotoyogi": "ROIS",
 "CragCrawler": None, "Crawl4AI": None, "Crawlspace": "Crawlspace", "Cursor": "Anysphere (Cursor)",
 "Datenbank Crawler": None, "DeepSeekBot": "DeepSeek", "Devin": "Devin AI", "Diffbot": "Diffbot",
 "Diffbot-User": "Diffbot", "DoubaoBot": "ByteDance", "DuckAssistBot": "DuckDuckGo", "Echobot Bot": "Echobox",
 "EchoboxBot": "Echobox", "ERNIEBot": "Baidu", "ExaBot": "Exa", "ExaSearchBot": "Exa", "FacebookBot": "Meta",
 "facebookexternalhit": "Meta", "Factset_spyderbot": "FactSet", "FirecrawlAgent": "Firecrawl", "FriendlyCrawler": None,
 "GeistHaus-PageFetcher": None, "Gemini-Deep-Research": "Google", "Google-Agent": "Google",
 "Google-CloudVertexBot": "Google", "Google-Extended": "Google", "Google-Firebase": "Google",
 "Google-Gemini-CLI": "Google", "Google-NotebookLM": "Google", "GoogleAgent-Mariner": "Google",
 "GoogleAgent-URLContext": "Google", "GoogleOther": "Google", "GoogleOther-Image": "Google",
 "GoogleOther-Video": "Google", "GPTBot": "OpenAI", "GrokAppAndroid": "xAI", "GrokBot": "xAI", "HenkBot": "Valyu",
 "Hunyuan": "Tencent", "iAskBot": "iAsk", "iaskspider": "iAsk", "iaskspider/2.0": "iAsk", "ICC-Crawler": "NICT",
 "ImagesiftBot": "ImageSift", "imageSpider": None, "img2dataset": None, "ISSCyberRiskCrawler": "ISS-Corporate",
 "kagi-fetcher": "Kagi", "Kangaroo Bot": "Kangaroo LLM", "KeenableBot": "Keenable", "Kimi-Agent": "Moonshot AI",
 "Kimi-SearchBot": "Moonshot AI", "Kimi-User": "Moonshot AI", "KimiBot": "Moonshot AI", "KlaviyoAIBot": "Klaviyo",
 "KunatoCrawler": None, "laion-huggingface-processor": "LAION", "LAIONDownloader": "LAION", "LCC": None,
 "Lightpanda": None, "LinerBot": "Liner", "Linguee Bot": "Linguee", "LinkupBot": "Linkup", "Manus-User": "Manus",
 "meta-externalagent": "Meta", "Meta-ExternalAgent": "Meta", "meta-externalfetcher": "Meta",
 "Meta-ExternalFetcher": "Meta", "meta-webindexer": "Meta", "MistralAI-Index": "Mistral AI",
 "MistralAI-Training": "Mistral AI", "MistralAI-User": "Mistral AI", "MistralAI-User/1.0": "Mistral AI",
 "MoonshotBot": "Moonshot AI", "Mozilla-Tabstack": "Mozilla", "MyCentralAIScraperBot": None, "NagetBot": "Naget",
 "nbot": "NBot", "netEstate Imprint Crawler": "netEstate", "newsai": None, "NotebookLM": "Google",
 "NovaAct": "Amazon", "OAI-AdsBot": "OpenAI", "OAI-SearchBot": "OpenAI", "omgili": "Webz.io", "omgilibot": "Webz.io",
 "OpenAI": "OpenAI", "opencode": None, "Operator": "OpenAI", "PanguBot": "Huawei", "Panscient": "Panscient",
 "panscient.com": "Panscient", "Perplexity-User": "Perplexity", "PerplexityBot": "Perplexity", "PetalBot": "Huawei",
 "PhindBot": "Phind", "Poggio-Citations": "Poggio", "Poseidon Research Crawler": "Poseidon Research",
 "qodercli": "Qoder", "QualifiedBot": "Qualified", "Querit-SearchBot": "Querit", "QueritBot": "Querit",
 "QuillBot": "QuillBot", "quillbot.com": "QuillBot", "QwenBot": "Alibaba", "Reflectionbot": "Reflection AI",
 "SBIntuitionsBot": "SB Intuitions", "Scrapy": "Zyte", "SemrushBot-OCOB": "Semrush", "SemrushBot-SWA": "Semrush",
 "Shap-User": "Parallel", "ShapBot": "Parallel", "Sidetrade indexer bot": "Sidetrade", "Spider": None,
 "TavilyBot": "Tavily", "Terra Cotta": "Ceramic AI", "TerraCotta": "Ceramic AI", "Thinkbot": "Thinkbot",
 "TikTokSpider": "ByteDance", "Timpibot": "Timpi", "TongyiBot": "Alibaba", "Trae": "ByteDance", "TwinAgent": "Twin",
 "UseAI": None, "VelenPublicWebCrawler": "Velen", "WARDBot": "WEBSPARK", "Webzio-Extended": "Webz.io",
 "webzio-extended": "Webz.io", "wpbot": "QuantumCloud", "WRTNBot": "Wrtn", "xAI-Grok": "xAI", "YaK": "Meltwater",
 "YandexAdditional": "Yandex", "YandexAdditionalBot": "Yandex", "YiBot": "01.AI", "YiyanBot": "Baidu",
 "YouBot": "You.com", "ZanistaBot": None,
}
NOTE = {k: "operator named, but the entry gives no web domain (not guessed)" for k in
        ("AgentDataBot", "CragCrawler", "Datenbank Crawler", "GeistHaus-PageFetcher", "UseAI")}
NOTE.update({k: "open-source client software run by anyone; no single operator" for k in
        ("Crawl4AI", "img2dataset", "Lightpanda", "opencode")})
# Radar operator string -> canonical operator (only where the spelling differs)
RALIAS = {"Brave Software, Inc.": "Brave", "Liner Bot": "Liner", "Research Organization of Information and Systems": "ROIS",
          "Qualified.com, Inc.": "Qualified", "Kernel Technologies, Inc.": "Kernel", "spotify": "Spotify",
          "Bixel, Inc.": "Bixel", "Exa": "Exa", "You.com": "You.com", "Lyrenth": "Lyrenth", "Channel3": "Channel3",
          "Mistral AI": "Mistral AI", "Moonshot AI": "Moonshot AI", "Reflection AI": "Reflection AI"}
# canonical operator -> operator's own web domains (apex first; extra = product/brand domains)
DOM = {
 "AddSearch": "addsearch.com", "The Agent Times": "theagenttimes.com", "Ai2": "allenai.org", "aiHit": "aihitdata.com",
 "Lyrenth": "lyrenth.com", "Amazon": "amazon.com aws.amazon.com", "Andi": "andisearch.com", "Direqt": "direqt.ai",
 "Anthropic": "anthropic.com claude.ai claude.com", "Apify": "apify.com", "Apple": "apple.com",
 "Atlassian": "atlassian.com", "Awario": "awario.com", "Microsoft": "microsoft.com bing.com", "Big Sur AI": "bigsur.ai",
 "Bixel": "bixel.com", "Brave": "brave.com search.brave.com", "Bright Data": "brightdata.com",
 "BuddyBotLearning": "buddybotlearning.com", "ByteDance": "bytedance.com", "Common Crawl": "commoncrawl.org",
 "Channel3": "trychannel3.com", "Zhipu AI": "zhipuai.cn chatglm.cn", "OpenAI": "openai.com chatgpt.com",
 "Cloudflare": "cloudflare.com", "Google": "google.com", "GitHub": "github.com", "Cohere": "cohere.com",
 "ROIS": "rois.ac.jp", "Crawlspace": "crawlspace.dev", "Anysphere (Cursor)": "cursor.com", "DeepSeek": "deepseek.com",
 "Devin AI": "devin.ai cognition.ai", "Diffbot": "diffbot.com", "DuckDuckGo": "duckduckgo.com",
 "Echobox": "echobox.com", "Baidu": "baidu.com", "Exa": "exa.ai", "Meta": "meta.com facebook.com",
 "FactSet": "factset.com", "Firecrawl": "firecrawl.dev", "xAI": "x.ai grok.com", "Valyu": "valyu.ai",
 "Tencent": "tencent.com", "iAsk": "iask.ai", "NICT": "nict.go.jp", "ImageSift": "imagesift.com",
 "ISS-Corporate": "iss-cyber.com", "Kagi": "kagi.com", "Kangaroo LLM": "kangaroollm.com.au", "Keenable": "keenable.ai",
 "Moonshot AI": "moonshot.ai kimi.com", "Klaviyo": "klaviyo.com", "LAION": "laion.ai", "Liner": "liner.com",
 "Linguee": "linguee.com", "Linkup": "linkup.so", "Manus": "manus.im", "Mistral AI": "mistral.ai",
 "Mozilla": "mozilla.org tabstack.ai", "Naget": "naget.ai", "NBot": "nbot.ai", "netEstate": "netestate.de",
 "Webz.io": "webz.io", "Huawei": "huawei.com aspiegel.com", "Panscient": "panscient.com", "Perplexity": "perplexity.ai",
 "Phind": "phind.com", "Poggio": "poggio.io", "Poseidon Research": "poseidonresearch.com", "Qoder": "qoder.com",
 "Qualified": "qualified.com", "Querit": "querit.ai", "QuillBot": "quillbot.com", "Alibaba": "alibaba.com qwen.ai",
 "Reflection AI": "reflection.ai", "SB Intuitions": "sbintuitions.co.jp", "Zyte": "zyte.com", "Semrush": "semrush.com",
 "Parallel": "parallel.ai", "Sidetrade": "sidetrade.com", "Tavily": "tavily.com", "Ceramic AI": "ceramic.ai",
 "Thinkbot": "thinkbot.agency", "Timpi": "timpi.io", "Twin": "twin.so", "Velen": "velen.io", "WEBSPARK": "ward.ai",
 "QuantumCloud": "quantumcloud.com", "Wrtn": "wrtn.ai", "Meltwater": "meltwater.com", "Yandex": "yandex.com yandex.ru",
 "01.AI": "01.ai", "You.com": "you.com",
 # Radar-only operators (domain from Radar's operatorUrl)
 "A1": "aone.dev", "ADP": "adp.com", "AGI, Inc": "theagi.company agi.tech", "Alphalens": "alphalensbot.com",
 "Anchor": "anchorbrowser.io", "Atticus Health": "atticushealth.com", "Borderxlab (Nubestore AI)": "nubestore.ai",
 "Brandwatch": "brandwatch.com", "Browserbase": "browserbase.com", "Caiged AI Limited": "citeworth.com",
 "Catalog": "getcatalog.ai", "Cerebro Technologies, Inc.": "cerebro.one", "Citibot": "citibot.io",
 "Cledara": "cledara.com cledara.ai", "codire GmbH": "it-recht-kanzlei.de", "Firmly": "firmly.ai",
 "FishBot": "fish.audio", "Gemmetric LLC": "gemmetric.ai", "h+h lab": None, "Hark": "hark.com", "Henry Labs": "henrylabs.ai",
 "HIFI": "hi.fi", "Hikari Software": "hikashop.com", "HivePoint, Inc.": "navu.co", "InspiMark LLC": "inspimark.com",
 "InstaClaw": "instaclaw.io", "Instant Paper": "instapaper.com", "Inteso Group BV": "chathive.app",
 "Kernel": "kernel.sh", "Lane": "getonlane.com", "Make.com": "make.com", "Milten": "milten.io",
 "MrScraper": "mrscraper.com", "Nava Labs": "navateam.com", "nippan.org": "nippan.org", "Novellum": "novellum.ai",
 "OBSIDIAAN TECH S.L": "primafonte.org", "Payhawk": "payhawk.com", "PayPal": "paypal.com", "Pulso360": "pulso360clouds.com",
 "QA.tech": "qa.tech", "Retool": "retool.com", "Riggle": "riggleapi.com", "Rye": "rye.com rye.xyz",
 "Scovant": "scovant.com", "Selectika": "selectika.com", "Shopify": "shopify.com", "Spark451, Inc. (Element451)": "element451.com",
 "Spotify": "spotify.com", "Stelar Digital LLC": "aistoreshopper.com", "Strivve": "strivve.com", "Vardia": "vardia.co",
 "Visually.io": "visually.io", "Wappalyzer": "wappalyzer.com", "Webmesh": "webmesh.ai", "Willow Intelligence": "willow.chat",
 "Zipchat Inc": "zipchat.ai",
}
# hosts named by the operator's own published crawler docs (not via Radar)
DOCHOST = {"Google": ["agent.bot.goog"]}  # developers.google.com/crawling/docs/crawlers-fetchers/web-bot-auth

ops, excluded = {}, []
for ua, v in robots.items():
    op = UA.get(ua, "UNMAPPED")
    if op is None or op == "UNMAPPED":
        excluded.append((ua, "ai.robots.txt", NOTE.get(ua, "no operator named in the entry") if op is None else "unmapped"))
        continue
    ops.setdefault(op, {"src": set(), "hosts": {}})["src"].add("ai.robots.txt")
for b in radar:
    op = RALIAS.get(b["operator"], b["operator"]) if b.get("operator") else None
    if b["category"] in AI_CATS:
        if op is None or DOM.get(op, "x") is None:
            excluded.append((b["name"], "radar", "no operator domain in Radar entry")); continue
        ops.setdefault(op, {"src": set(), "hosts": {}})["src"].add("radar-ai")
for b in radar:  # documented hosts: any Radar entry (any category) for an operator in the population
    op = RALIAS.get(b["operator"], b["operator"]) if b.get("operator") else None
    if op in ops and b.get("signatureAgentUrl"):
        ops[op]["hosts"].setdefault(b["signatureAgentUrl"], "radar-signature-agent")
rows = []
for op, o in sorted(ops.items(), key=lambda t: t[0].lower()):
    for h in DOCHOST.get(op, []):
        o["hosts"].setdefault(WK % h, "operator-doc")
    doms = (DOM.get(op) or "").split()
    if not doms:
        excluded.append((op, "/".join(sorted(o["src"])), "no domain mapped")); continue
    for i, d in enumerate(doms):
        o["hosts"].setdefault(WK % d, "apex" if i == 0 else "brand-domain")
        o["hosts"].setdefault(WK % ("www." + d), "www" if i == 0 else "brand-www")
    for u, basis in o["hosts"].items():
        rows.append((op, "+".join(sorted(o["src"])), u, basis))
with open(out, "w", newline="") as f:
    w = csv.writer(f); w.writerow(["operator", "sources", "url", "basis"]); w.writerows(rows)
with open(out.replace(".csv", "-excluded.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["name", "source", "reason"]); w.writerows(excluded)
print(len(ops), "operators,", len(rows), "targets,", len(excluded), "excluded", file=sys.stderr)

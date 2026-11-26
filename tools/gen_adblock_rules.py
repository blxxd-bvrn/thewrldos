#!/usr/bin/env python3
"""The ad blocker's rules for the WebKit browser (wrldweb), in WebKit's content blocker format
(the JSON Safari's content blockers use), made from the same lists as The WRLD Browser's own
ad blocker (rust/src/adblock.rs) plus rules for video sites. wrldweb adds the user's
C:\\WRLD\\adblock.txt to them when it starts.

usage: gen_adblock_rules.py rust/src/adblock.rs out.json"""
import json, re, sys

src = open(sys.argv[1], encoding='utf-8').read()

def array(name):
    m = re.search(r'pub const %s: &\[&\[u8\]\] = &\[(.*?)\];' % name, src, re.S)
    return re.findall(r'b"((?:[^"\\]|\\.)*)"', m.group(1)) if m else []

hosts = array('BUILTIN_HOSTS')
paths = array('BUILTIN_PATHS')
hide = array('BUILTIN_HIDE')
hide_prefix = array('BUILTIN_HIDE_PREFIX')

# more ad and tracking servers than the built-in browser needs (it cannot run their scripts anyway)
hosts += ['googleads.g.doubleclick.net', 'static.doubleclick.net', 'ad.doubleclick.net', 'adservice.google.ca',
          'tpc.googlesyndication.com', 'partner.googleadservices.com', 'fundingchoicesmessages.google.com',
          'cdn.taboola.com', 'trc.taboola.com', 'widgets.outbrain.com', 'securepubads.g.doubleclick.net',
          'c.amazon-adsystem.com', 'aax.amazon-adsystem.com', 'ads.pubmatic.com', 'cdn.krxd.net', 'cdn.adsafeprotected.com',
          'ib.adnxs.com', 'acdn.adnxs.com', 'ads.yieldmo.com', 'adsystem.com', 'adnxs-simple.com', 'smartclip.net',
          'innovid.com', 'flashtalking.com', 'sizmek.com', 'advertising.amazon.com', 'zemanta.com', 'yandexadexchange.net',
          'an.yandex.ru', 'mc.yandex.ru', 'adfox.ru', 'pop-ads.com', 'popcash.net', 'adsco.re', 'ad-maven.com',
          'clickadu.com', 'a-ads.com', 'coinzilla.com', 'bidvertiser.com', 'udmserve.net', 'ezoic.net', 'ezojs.com']
hosts = sorted(set(h.lower() for h in hosts))

# YouTube serves its ads and their reports from its own servers: these paths are only ads
youtube_paths = [r'youtube\.com/api/stats/ads', r'youtube\.com/pagead/', r'youtube\.com/ptracking', r'youtube\.com/get_midroll_',
                 r'youtube\.com/api/stats/atr', r'youtube\.com/pcs/activeview', r'youtube\.com/youtubei/v1/log_event\?.*adformat',
                 r'googlevideo\.com/initplayback\?.*oad', r'youtube\.com/generate_204\?.*ad_', r'youtube\.com/csi_204\?.*ad']

rules = []
for h in hosts:
    rules.append({'trigger': {'url-filter': r'^[a-z]+://([^/:]*\.)?' + re.escape(h) + r'[/:]'}, 'action': {'type': 'block'}})
for p in paths:
    rules.append({'trigger': {'url-filter': re.escape(p), 'resource-type': ['script', 'image', 'raw', 'style-sheet', 'media', 'popup']},
                  'action': {'type': 'block'}})
for p in youtube_paths:
    rules.append({'trigger': {'url-filter': p}, 'action': {'type': 'block'}})

# elements that hold ads: whole class or id names, and common prefixes
sel = []
for w in hide:
    sel += ['.' + w, '#' + w]
for p in hide_prefix:
    sel += ['[id^="%s"]' % p, '[class^="%s"]' % p]
sel += ['ins.adsbygoogle', 'iframe[src*="doubleclick.net"]', 'iframe[id^="google_ads_iframe"]', 'div[data-ad-slot]',
        '[data-google-query-id]', 'a[href*="doubleclick.net/"]', 'amp-ad', 'amp-embed[type="taboola"]']
for i in range(0, len(sel), 60):                    # WebKit wants selector lists of moderate size
    rules.append({'trigger': {'url-filter': '.*'}, 'action': {'type': 'css-display-none', 'selector': ', '.join(sel[i:i + 60])}})
yt = ['ytd-ad-slot-renderer', 'ytd-in-feed-ad-layout-renderer', 'ytd-banner-promo-renderer', 'ytd-statement-banner-renderer',
      'ytd-promoted-sparkles-web-renderer', 'ytd-promoted-sparkles-text-search-renderer', 'ytd-display-ad-renderer',
      'ytd-compact-promoted-video-renderer', 'ytd-promoted-video-renderer', 'ytd-search-pyv-renderer', 'ytd-video-masthead-ad-v3-renderer',
      'ytd-primetime-promo-renderer', 'ytd-merch-shelf-renderer', 'ytd-engagement-panel-section-list-renderer[target-id="engagement-panel-ads"]',
      '#player-ads', '#masthead-ad', '.ytp-ad-overlay-container', '.ytp-ad-message-container', '.ytd-ad-slot-renderer',
      'ytd-rich-item-renderer:has(ytd-ad-slot-renderer)', 'ytd-enforcement-message-view-model', 'tp-yt-paper-dialog:has(ytd-enforcement-message-view-model)']
rules.append({'trigger': {'url-filter': r'^https?://([^/]*\.)?youtube\.com/'}, 'action': {'type': 'css-display-none', 'selector': ', '.join(yt)}})

with open(sys.argv[2], 'w') as f:
    json.dump(rules, f, separators=(',', ':'))
print('%s: %d rules (%d servers)' % (sys.argv[2], len(rules), len(hosts)))

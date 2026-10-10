"""Digest a saved Ozon .mhtml product page: title + connector/spec snippets."""
import email, re, sys
from email import policy

KEYWORDS = ("U.FL", "W.FL", "MHF", "IPEX", "IPX", "разъём", "разъем",
            "коннектор", "connector", "dBi", "2.4", "5.8", "кабел",
            "cable", "антенн", "antenna")

for path in sys.argv[1:]:
    with open(path, "rb") as f:
        msg = email.message_from_binary_file(f, policy=policy.default)
    html = None
    for part in msg.walk():
        if part.get_content_type() == "text/html":
            html = part.get_content()
            break
    if html is None:
        print(f"!! no html part in {path}")
        continue

    m = re.search(r"<title>(.*?)</title>", html, re.S | re.I)
    page_text = re.sub(r"<(script|style).*?</\1>", " ", html, flags=re.S | re.I)
    page_text = re.sub(r"<[^>]+>", " ", page_text)
    page_text = re.sub(r"&nbsp;?", " ", page_text)
    page_text = re.sub(r"\s+", " ", page_text)

    print("=" * 100)
    print("TITLE:", (m.group(1) if m else "?").strip()[:160])

    seen = set()
    shown = 0
    for kw in KEYWORDS:
        for m2 in re.finditer(re.escape(kw), page_text, re.I):
            s = max(0, m2.start() - 90)
            snip = page_text[s:m2.end() + 110].strip()
            key = snip[:60].lower()
            if key in seen:
                continue
            seen.add(key)
            print(f"  …{snip}…")
            shown += 1
            if shown > 45:
                break
        if shown > 45:
            break

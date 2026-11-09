"""Text cleaning and streaming sentence splitting for TTS."""
from __future__ import annotations

import re

# CJK ideographs, kana, CJK punctuation and fullwidth forms.
_CJK = r"　-〿぀-ヿ㐀-䶿一-鿿豈-﫿＀-￯"

_EMOJI_RE = re.compile(
    r"[\U0001F000-\U0001FAFF☀-➿⌀-⏿⬀-⯿"
    r"️‍⃣\U000E0020-\U000E007F\U0001F1E6-\U0001F1FF]"
)

_FENCE_CLOSED_RE = re.compile(r"```.*?```", re.DOTALL)
_FENCE_OPEN_RE = re.compile(r"```.*\Z", re.DOTALL)
_INLINE_CODE_RE = re.compile(r"`([^`\n]*)`")
_IMAGE_RE = re.compile(r"!\[[^\]\n]*\]\([^)\n]*\)")
_LINK_RE = re.compile(r"\[([^\]\n]*)\]\([^)\n]*\)")
_AUTOLINK_RE = re.compile(r"<https?://[^>\s]*>")
_HTML_TAG_RE = re.compile(r"</?[A-Za-z][^<>\n]*>")
_URL_RE = re.compile(r"(?:https?://|www\.)[^\s<>\"'，。、；！？）》」』】\])]+")
_HR_RE = re.compile(r"^[ \t]*([-*_])(?:[ \t]*\1){2,}[ \t]*$", re.MULTILINE)
_TABLE_SEP_RE = re.compile(
    r"^[ \t]*\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)+\|?[ \t]*$", re.MULTILINE
)
_HEADING_RE = re.compile(r"^[ \t]*#{1,6}[ \t]+", re.MULTILINE)
_QUOTE_RE = re.compile(r"^[ \t]*(?:>[ \t]?)+", re.MULTILINE)
_BULLET_RE = re.compile(r"^[ \t]*(?:[-*+]|\d+\.)[ \t]+", re.MULTILINE)
_BOLD_STAR_RE = re.compile(r"\*\*(.+?)\*\*")
_BOLD_UNDER_RE = re.compile(r"__(.+?)__")
_STRIKE_RE = re.compile(r"~~(.+?)~~")
_ITALIC_STAR_RE = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])")
_ITALIC_UNDER_RE = re.compile(r"(?<!\w)_(?!\s)(.+?)(?<!\s)_(?!\w)")
_CJK_SPACE_RE = re.compile(rf"(?<=[{_CJK}])\s+(?=[{_CJK}])")
_WS_RE = re.compile(r"\s+")

_INLINE_CODE_MAX = 20
_MEME_MARKER_RE = re.compile(r"&&[^&\r\n]{1,100}&&")
_MEME_PARTIAL_RE = re.compile(r"[^&\r\n]{0,100}&?")
_MEME_UNFINISHED_RE = re.compile(r"&&[A-Za-z_][A-Za-z0-9_-]{0,99}&?")


def _strip_meme_markers(text: str, *, final: bool) -> tuple[str, str]:
    """Return visible text and a bounded marker suffix awaiting the next delta."""
    output: list[str] = []
    while text:
        start = text.find("&&")
        if start < 0:
            if not final and text.endswith("&"):
                return "".join(output) + text[:-1], "&"
            return "".join(output) + text, ""
        output.append(text[:start])
        text = text[start:]
        marker = _MEME_MARKER_RE.match(text)
        if marker:
            output.append(" ")
            text = text[marker.end():]
            continue
        if not final and _MEME_PARTIAL_RE.fullmatch(text[2:]):
            return "".join(output), text
        if final and _MEME_UNFINISHED_RE.fullmatch(text):
            return "".join(output), ""
        output.append("&&")
        text = text[2:]
    return "".join(output), ""


def _inline_code(match: re.Match[str]) -> str:
    inner = match.group(1)
    return inner if len(inner) <= _INLINE_CODE_MAX else " "


_BARE_BREAK_RE = re.compile(r"(?<=[^\s。！？!?；;，,、：:.…])[ \t]*\n\s*(?=\S)")


def clean_for_tts(text: str) -> str:
    """Strip markup, URLs, emoji and other non-speakable parts; normalise whitespace."""
    text, _ = _strip_meme_markers(text, final=True)
    text = _FENCE_CLOSED_RE.sub(" ", text)
    text = _FENCE_OPEN_RE.sub(" ", text)  # unclosed fence: drop the rest
    text = _INLINE_CODE_RE.sub(_inline_code, text)
    text = _IMAGE_RE.sub(" ", text)
    text = _LINK_RE.sub(r"\1", text)
    text = _AUTOLINK_RE.sub(" ", text)
    text = _HTML_TAG_RE.sub(" ", text)
    text = _URL_RE.sub(" ", text)
    text = _HR_RE.sub("", text)
    text = _TABLE_SEP_RE.sub("", text)
    text = _HEADING_RE.sub("", text)
    text = _QUOTE_RE.sub("", text)
    text = _BULLET_RE.sub("", text)
    text = text.replace("|", " ")
    text = _BOLD_STAR_RE.sub(r"\1", text)
    text = _BOLD_UNDER_RE.sub(r"\1", text)
    text = _STRIKE_RE.sub(r"\1", text)
    text = _ITALIC_STAR_RE.sub(r"\1", text)
    text = _ITALIC_UNDER_RE.sub(r"\1", text)
    text = _EMOJI_RE.sub(" ", text)
    # a line break with no punctuation before it (headings, list items) becomes a pause
    text = _BARE_BREAK_RE.sub("，", text.strip())
    text = _WS_RE.sub(" ", text).strip()
    return _CJK_SPACE_RE.sub("", text)


_HARD_TERMINATORS = frozenset("。！？!?；;…\n")
_CLOSERS = frozenset("”」』）)\"'")
_SOFT_BREAKS = frozenset("，,、：: ")


class SentenceSplitter:
    """Buffers streamed text and yields sentences as soon as they are complete.

    Returned sentences are raw (not cleaned); callers run `clean_for_tts` on each.
    """

    def __init__(self, min_chars: int = 4, max_chars: int = 60):
        self._min_chars = min_chars
        self._max_chars = max_chars
        self._buf = ""
        self._carry = ""  # raw text of too-short sentences waiting to merge forward
        self._marker_pending = ""

    def feed(self, delta: str) -> list[str]:
        visible, self._marker_pending = _strip_meme_markers(self._marker_pending + delta, final=False)
        self._buf += visible
        out: list[str] = []
        while True:
            cut = self._find_cut(final=False)
            if cut is None:
                break
            piece, self._buf = self._buf[:cut], self._buf[cut:]
            self._emit(piece, out, final=False)
        return out

    def flush(self) -> list[str]:
        visible, self._marker_pending = _strip_meme_markers(self._marker_pending, final=True)
        self._buf += visible
        out: list[str] = []
        while True:
            cut = self._find_cut(final=True)
            if cut is None:
                break
            piece, self._buf = self._buf[:cut], self._buf[cut:]
            self._emit(piece, out, final=True)
        rest, self._buf = self._buf, ""
        self._emit(rest, out, final=True)
        return out

    def _emit(self, piece: str, out: list[str], final: bool) -> None:
        raw = self._carry + piece
        self._carry = ""
        if not raw.strip():
            return
        if not final and len(clean_for_tts(raw)) < self._min_chars:
            self._carry = raw
            return
        out.append(raw.strip())

    def _is_terminator(self, i: int) -> bool:
        buf = self._buf
        c = buf[i]
        if c in _HARD_TERMINATORS:
            return True
        if c == ".":
            if i > 0 and buf[i - 1].isdigit():
                return False
            return i + 1 < len(buf) and buf[i + 1].isspace()
        return False

    def _find_cut(self, final: bool) -> int | None:
        """Return the end index of the next sentence in the buffer, or None to wait."""
        buf = self._buf
        n = len(buf)
        in_fence = False
        term_end: int | None = None
        waiting = False

        i = 0
        while i < n:
            if buf.startswith("```", i):
                in_fence = not in_fence
                i += 3
                continue
            if in_fence:
                i += 1
                continue
            if self._is_terminator(i):
                j = i + 1
                while j < n and (buf[j] in _HARD_TERMINATORS or buf[j] in _CLOSERS):
                    j += 1
                if j == n and not final:
                    # Terminator run may continue in the next feed; wait for more text.
                    waiting = True
                    break
                term_end = j
                break
            i += 1

        if term_end is not None and term_end <= self._max_chars:
            return term_end
        if in_fence:
            return None  # never cut inside a fenced code block
        if waiting and n <= self._max_chars:
            return None
        if n > self._max_chars:
            window = buf[: self._max_chars]
            for k in range(len(window) - 1, -1, -1):
                if window[k] in _SOFT_BREAKS:
                    return k + 1
            return self._max_chars
        return None

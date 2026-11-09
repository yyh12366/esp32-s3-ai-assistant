"""Tests for TTS text cleaning and sentence splitting."""
from __future__ import annotations

import pytest

from relay.textproc import SentenceSplitter, clean_for_tts


def split_all(text: str, **kw) -> list[str]:
    s = SentenceSplitter(**kw)
    return s.feed(text) + s.flush()


def split_streamed(text: str, **kw) -> list[str]:
    s = SentenceSplitter(**kw)
    out: list[str] = []
    for ch in text:
        out.extend(s.feed(ch))
    out.extend(s.flush())
    return out


# --- splitting -------------------------------------------------------------

def test_chinese_sentences_split():
    assert split_all("你好世界。今天天气很好！真的吗？好的，我们去公园吧。") == [
        "你好世界。",
        "今天天气很好！",
        "真的吗？",
        "好的，我们去公园吧。",
    ]


def test_english_sentences_split():
    assert split_all("Hello there, friend. How are you today? Fine!") == [
        "Hello there, friend.",
        "How are you today?",
        "Fine!",
    ]


def test_decimal_and_version_not_split():
    out = split_all("圆周率是3.14，版本v1.2很好。")
    assert out == ["圆周率是3.14，版本v1.2很好。"]


def test_terminator_run_stays_attached():
    assert split_all("好的！？ 然后走了。") == ["好的！？", "然后走了。"]
    assert split_all("真的假的?! 好。") == ["真的假的?!", "好。"]


def test_closing_quote_attached():
    assert split_all("他说“你好。”然后走了。") == ["他说“你好。”", "然后走了。"]


def test_code_block_not_split_and_removed_by_clean():
    text = "先看代码```py\nx = 1. y = 2! z = 3?\n```后面的话很长。"
    pieces = split_all(text)
    # The fence contains sentence-like punctuation but must stay in one piece.
    assert len(pieces) == 1
    assert "x = 1. y = 2! z = 3?" in pieces[0]
    cleaned = " ".join(clean_for_tts(p) for p in pieces)
    assert "x = 1" not in cleaned
    assert "后面的话很长" in cleaned


def test_streaming_matches_one_shot():
    text = (
        "你好世界。今天天气很好！真的吗？好的，我们去公园吧。"
        "圆周率是3.14，版本v1.2很好。Hello there, friend. How are you? Fine!"
        "他说“你好。”然后走了。"
        "```py\nx = 1. y = 2\n```\n代码后面的话。"
        "嗯。好。"
    )
    assert split_streamed(text) == split_all(text)


def test_streaming_matches_one_shot_long_text():
    text = "这是一段没有任何标点符号的很长很长的文字用来测试最大长度切分的逻辑是否正确地工作在空格处或者逗号处" * 3
    assert split_streamed(text) == split_all(text)


def test_min_chars_merges_short_sentence_forward():
    # "嗯。" is shorter than min_chars=4, so it is merged into the following sentence.
    assert split_all("嗯。今天天气很好。", min_chars=4) == ["嗯。今天天气很好。"]


def test_min_chars_short_tail_emitted_on_flush():
    s = SentenceSplitter(min_chars=4)
    out = s.feed("好的。")
    assert out == []
    assert s.flush() == ["好的。"]


def test_max_chars_cut_at_soft_break():
    text = "甲" * 30 + "，" + "乙" * 40 + "。"
    out = split_all(text, max_chars=50)
    assert out[0] == "甲" * 30 + "，"
    assert out[1].startswith("乙")
    assert "".join(out).replace(" ", "") == text


def test_max_chars_hard_cut_without_break():
    text = "丙" * 70 + "。"
    out = split_all(text, max_chars=60)
    assert out[0] == "丙" * 60
    assert "".join(out) == text


def test_empty_and_whitespace_dropped():
    assert split_all("   \n  ") == []
    assert split_all("") == []


# --- cleaning --------------------------------------------------------------

def test_clean_links_images_urls():
    text = "看 [这里](https://a.com/x) ![图](pic.png) 或 https://b.com/y 还有 www.c.com 结束"
    assert clean_for_tts(text) == "看这里或还有结束"


def test_clean_autolink_and_html():
    assert clean_for_tts("<https://x.org> 文字<br>继续") == "文字继续"


def test_clean_markdown_markers():
    text = "# 标题\n> 引用\n- 列表一\n1. 列表二\n**粗体** __下划线__ *斜体* ~~删除~~ _斜_"
    cleaned = clean_for_tts(text)
    assert cleaned == "标题，引用，列表一，列表二，粗体下划线斜体删除斜"


def test_clean_inline_code_length_rule():
    assert clean_for_tts("用 `ls` 命令") == "用 ls 命令"
    assert clean_for_tts("路径 `abcdefghijklmnopqrstuvwxyz0123` end") == "路径 end"


def test_clean_fenced_block_removed():
    assert clean_for_tts("front\n```python\nprint('hi')\n```\nback") == "front，back"


def test_clean_table_and_hr():
    text = "| 名字 | 值 |\n|---|---|\n| 甲 | 1 |\n---\n结束"
    assert clean_for_tts(text) == "名字值，甲 1，结束"


def test_clean_emoji_removed():
    assert clean_for_tts("好的 😀👍🏽 ❤️ 🇨🇳 谢谢") == "好的谢谢"
    assert clean_for_tts("hi 😀 there") == "hi there"


def test_clean_keeps_cjk_punctuation_and_removes_cjk_spaces():
    assert clean_for_tts("你 好，世界！ Hello 世界") == "你好，世界！ Hello 世界"


@pytest.mark.parametrize("text,expected", [
    ("&&sigh&&", ""),
    ("知道了&&sigh&&。", "知道了。"),
    ("hello&&happy&&world", "hello world"),
    ("&&baka&&&&sigh&&", ""),
    ("&&sigh", ""),
    ("a && b", "a && b"),
    ("A & B", "A & B"),
])
def test_clean_removes_meme_markers(text, expected):
    assert clean_for_tts(text) == expected


@pytest.mark.parametrize("text", [
    "知道了。&&sigh&&我继续说。",
    "hello&&happy&&world.",
    "甲" * 59 + "&&sigh&&" + "乙" * 65,
    "甲" * 59 + "&&very_long_emotion_label&&" + "乙" * 65,
    "&&sigh&&&&baka&&",
    "我知道了&&sigh",
    "A & B。a && b",
])
def test_streamed_meme_markers_never_reach_sentences(text):
    expected = clean_for_tts(text)
    for cut in range(len(text) + 1):
        splitter = SentenceSplitter()
        pieces = splitter.feed(text[:cut]) + splitter.feed(text[cut:]) + splitter.flush()
        spoken = [clean_for_tts(piece) for piece in pieces]
        assert clean_for_tts("".join(spoken)) == expected
    spoken = [clean_for_tts(piece) for piece in split_streamed(text)]
    assert clean_for_tts("".join(spoken)) == expected

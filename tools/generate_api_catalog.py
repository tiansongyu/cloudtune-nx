#!/usr/bin/env python3
"""Generate audited API documentation from the exact GTK dependency checkout."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path, help="netease-cloud-music-api 2.2.0 checkout")
parser.add_argument("--gtk", type=Path, default=Path("../netease-cloud-music-gtk"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
text = (args.source / "src/lib.rs").read_text()
gtk = "\n".join(p.read_text() for p in args.gtk.glob("src/**/*.rs"))
matches = list(re.finditer(r"pub (async )?fn (\w+)(?:<[^>]*>)?\s*\(", text))
inputs = {
    "login": "username, password 或 password_md5",
    "login_cellphone": "phone, captcha, ctcode=86",
    "captcha": "phone, ctcode=86",
    "login_qr_create": "无",
    "login_qr_check": "key",
    "user_song_id_list": "uid",
    "user_song_list": "uid, offset=0, limit=30",
    "album_sublist": "offset=0, limit=30",
    "user_cloud_disk": "offset=0, limit=30",
    "song_list_detail": "songlist_id (别名 id), offset=0, limit=1000, n=1000",
    "songs_detail": "ids:正整数数组，1–1000项",
    "songs_url": "ids, br=320000",
    "songs_url_v1": "ids, level=standard, encodeType=mp3/flac",
    "like": "songid (别名 id), like=true",
    "like_v1": "songid (别名 id), like=true",
    "fm_trash": "songid (别名 id)",
    "search": "keywords (别名 s), types/type=1, offset=0, limit=30",
    "search_song": "keywords, offset=0, limit=30",
    "search_singer": "keywords, offset=0, limit=30",
    "search_album": "keywords, offset=0, limit=30",
    "search_songlist": "keywords, offset=0, limit=30",
    "search_lyrics": "keywords, offset=0, limit=30",
    "singer_songs": "id",
    "singer_all_songs": "id, order=hot, offset=0, limit=30",
    "new_albums": "area=ALL, offset=0, limit=30",
    "album": "album_id (别名 id)",
    "songlist_detail_dynamic": "songlist_id (别名 id)",
    "album_detail_dynamic": "album_id (别名 id)",
    "top_song_list": "cat=全部, order=hot, offset=0, limit=30",
    "top_song_list_highquality": "cat=全部, lasttime=0 (64位时间戳), limit=30",
    "top_songs": "list_id (别名 id), offset=0, limit=1000, n=1000",
    "song_lyric": "music_id (别名 id)",
    "song_list_like": "id, like=true",
    "album_like": "id, like=true",
    "homepage": "cursor=null",
    "banners": "clientType=pc",
    "download_img": "url, path, width=300, high=300",
    "download_song": "url, path",
    "user_radio_sublist": "offset=0, limit=30",
    "radio_program": "rid (别名 id), offset=0, limit=30",
    "playmode_intelligence_list": "sid (别名 song_id), pid (别名 playlist_id), count=1",
}
descriptions = {
    "fetch_xeapi_public_key": "取得并缓存 XEAPI 动态公钥",
    "login": "密码登录（邮箱或手机号）",
    "login_cellphone": "手机短信验证码登录",
    "captcha": "发送登录短信验证码",
    "login_qr_create": "取得登录二维码 key 与扫码 URL",
    "login_qr_check": "轮询扫码登录状态",
    "login_status": "当前账户、个人资料与登录状态",
    "logout": "退出登录并清除本机会话",
    "daily_task": "每日签到",
    "user_song_id_list": "用户喜欢歌曲的 ID 列表",
    "user_song_list": "用户创建及收藏的歌单",
    "album_sublist": "用户收藏专辑",
    "user_cloud_disk": "用户音乐云盘",
    "song_list_detail": "歌单详情、tracks 与完整 trackIds",
    "songs_detail": "批量歌曲详情与权限信息",
    "songs_url": "按码率取得播放 URL",
    "songs_url_v1": "按音质取得播放 URL、时效、格式与权限",
    "recommend_resource": "每日推荐歌单",
    "recommend_songs": "每日推荐歌曲",
    "personal_fm": "私人 FM",
    "like": "旧版歌曲喜欢/取消喜欢",
    "like_v1": "新版歌曲喜欢/取消喜欢",
    "fm_trash": "私人 FM 不喜欢",
    "search": "综合类型搜索",
    "search_song": "搜索歌曲",
    "search_singer": "搜索歌手",
    "search_album": "搜索专辑",
    "search_songlist": "搜索歌单",
    "search_lyrics": "按歌词搜索歌曲",
    "singer_songs": "歌手热门歌曲",
    "singer_all_songs": "歌手全部歌曲",
    "new_albums": "分地区新碟",
    "album": "专辑与曲目详情",
    "songlist_detail_dynamic": "歌单收藏与动态状态",
    "album_detail_dynamic": "专辑收藏与动态状态",
    "top_song_list": "分类热门/最新歌单",
    "top_song_list_highquality": "精品歌单与时间戳分页",
    "toplist": "全部排行榜",
    "top_songs": "排行榜歌曲（歌单详情别名）",
    "song_lyric": "原文、翻译及罗马音歌词",
    "song_list_like": "收藏/取消收藏歌单",
    "album_like": "收藏/取消收藏专辑",
    "homepage": "App 首页区块",
    "banners": "首页轮播内容",
    "download_img": "下载 CDN 图片，可请求缩略尺寸",
    "download_song": "下载已授权 CDN 音频",
    "user_radio_sublist": "用户订阅电台",
    "radio_program": "电台节目列表",
    "playmode_intelligence_list": "心动模式智能队列",
}
session = {
    "new": "ApiClient(ApiConfig)",
    "from_cookie_jar": "ApiConfig.cookie_file（libcurl Netscape Cookie 格式）",
    "cookie_jar": "内部 libcurl cookie engine；flush_cookies()/clear_session()",
    "set_proxy": "ApiClient::set_proxy() / ApiConfig.proxy",
}
mutations = {"login", "login_cellphone", "captcha", "login_qr_create", "login_qr_check",
             "logout", "daily_task", "like", "like_v1", "fm_trash", "song_list_like", "album_like"}
auth = {"user_song_id_list", "user_song_list", "album_sublist", "user_cloud_disk",
        "recommend_resource", "recommend_songs", "like", "like_v1", "fm_trash",
        "song_list_like", "album_like", "user_radio_sublist", "daily_task",
        "playmode_intelligence_list"}
entries = []
for i, match in enumerate(matches):
    name = match.group(2)
    if name in session:
        continue
    body = text[match.start():matches[i + 1].start() if i + 1 < len(matches) else text.index("\nfn build_cookie")]
    signature = body.split("{", 1)[0].strip()
    paths = list(dict.fromkeys(re.findall(r'"((?:/|https://)[^"]+)"', body)))
    if name == "fetch_xeapi_public_key":
        paths = ["https://interface.music.163.com/api/gorilla/anti/crawler/security/key/get"]
    if name == "login_qr_create":
        paths = ["/weapi/login/qrcode/unikey"]
    if name.startswith("search_"):
        paths = ["/weapi/search/get"]
    if name == "top_songs":
        paths = ["/weapi/v6/playlist/detail"]
    if name.startswith("download_"):
        paths = ["调用者提供的 HTTP(S) CDN URL"]
    protocol = "XEAPI" if name == "like_v1" else "EAPI" if name in {"songs_url", "songs_url_v1"} else \
        "GET" if name.startswith("download_") else "FORM+AES-256" if name == "fetch_xeapi_public_key" else "WEAPI"
    entries.append({
        "method": name,
        "description": descriptions[name],
        "cpp_arguments": inputs.get(name, "无"),
        "reference_paths": paths,
        "protocol": protocol,
        "http_method": "GET" if name.startswith("download_") else "POST",
        "requires_login": name in auth,
        "changes_account_or_session": name in mutations,
        "used_by_gtk": bool(re.search(r"\b" + re.escape(name) + r"\s*\(", gtk)),
        "reference_signature": signature,
        "reference_line": text[:match.start()].count("\n") + 1,
        "implementation": "ApiClient::call",
        "validation": "implemented; authenticated and hardware behavior require user/device testing",
    })
catalog = {
    "schema_version": 1,
    "reference": {
        "gtk": "netease-cloud-music-gtk 2.5.4",
        "api_url": "https://github.com/gmg137/netease-cloud-music-api",
        "tag": "2.2.0",
        "commit": "73ab46e5351fd3c31b67709ee1b4b918205b9e3e",
        "license": "GPL-3.0",
    },
    "count": len(entries),
    "session_helpers": session,
    "apis": entries,
}
out = root / "docs"
out.mkdir(exist_ok=True)
(out / "api_catalog.json").write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n")
lines = [
    "# 网易云音乐 API 全量清单",
    "",
    "本清单先于界面功能划分建立，目的是让服务能力、界面调用、测试范围可以逐项追踪。",
    "",
    "来源：`netease-cloud-music-gtk` 2.5.4 的 `Cargo.toml` 锁定了 "
    "[netease-cloud-music-api 2.2.0](https://github.com/gmg137/netease-cloud-music-api/tree/2.2.0)，"
    "commit `73ab46e5351fd3c31b67709ee1b4b918205b9e3e`。"
    "接口来自该开源客户端的一手代码，并非网易公开且保证兼容的商业 API。",
    "",
    f"范围：**{len(entries)} 个服务/下载方法 + 4 个会话辅助方法**。所有服务方法均可通过 "
    "`ApiClient::call(method, arguments)` 调用；返回服务原始 JSON。"
    "机器可读版本为 [api_catalog.json](api_catalog.json)。",
    "",
    "状态含义：表内为**代码实现范围**，不等于已用真实账户和 Switch 硬件验证。"
    "实时验证结果单独记录在 [API_VALIDATION.md](API_VALIDATION.md)。",
    "",
    "## 接口矩阵",
    "",
    "| 方法 | 用途 | C++ 参数（省略项采用默认值） | 协议 | 参考端点 |",
    "|---|---|---|---|---|",
]
for entry in entries:
    paths = "<br>".join("`" + p.replace("|", "\\|") + "`" for p in entry["reference_paths"])
    lines.append(f'| `{entry["method"]}` | {entry["description"]} | {entry["cpp_arguments"]} | {entry["protocol"]} | {paths} |')
lines += [
    "",
    "## 会话与加密",
    "",
    "| Rust 辅助方法 | Switch C++ 对应 |",
    "|---|---|",
    *[f"| `{k}` | {v} |" for k, v in session.items()],
    "",
    "- WEAPI：随机 16 字节会话密钥、两层 AES-128-CBC/PKCS#7、RSA-1024 无填充公钥运算。",
    "- EAPI：API 路径和请求体的 MD5 摘要、AES-128-ECB/PKCS#7；播放 URL 支持账户 cookie。",
    "- XEAPI：动态公钥握手、HMAC-SHA256、X25519、HKDF 兼容派生、AES-GCM 与 AES-ECB；用于新版喜欢接口。",
    "- 底层使用 libcurl、mbedTLS 2.28 和 zlib。TLS 验证保持启用；Switch 的 CA 文件由 ApiConfig.ca_bundle 指定。",
    "- 会话使用 `cookies.txt` 保存。扫码授权返回的 Set-Cookie 由 libcurl 保存，后续请求自动携带；退出清除会话。"
    "cookie 是账户凭据，不应提交到仓库或上传日志。",
    "- 扫码状态 800（失效）、801（等待）、802（已扫描）、803（授权完成）作为正常状态返回；"
    "HTTP/服务失败抛出带 code 的 ApiError。",
    "",
    "## 与参考实现的明确差异",
    "",
    "- 参考实现若把 WEAPI 加密体发送到 `/api/`，本项目对应使用 `/weapi/` 路径；EAPI/XEAPI 保留签名 `/api/` 路径。"
    "修复参考 logout 中重复拼接主机名的问题。",
    "- songs_detail 的 `c` 使用标准 JSON 数组编码，不再额外插入反斜杠。",
    "- login 默认接收明文 password 后在本地 MD5；已计算摘要可传 password_md5。不会把密码写入日志。",
    "- user_cloud_disk 采用可分页默认 30 首，避免一次请求 10000 首占用 Switch 内存；循环请求直到不足 limit 即可。",
    "- 精品歌单 lasttime 接受完整毫秒时间戳，不沿用参考实现的 u8 限制。",
    "- 有损音质默认请求 MP3 方便后台解码，其他音质请求 FLAC；可传 encodeType 覆盖。"
    "音质 level 为 standard / higher / exhigh / lossless / hires / jyeffect / sky / jymaster。"
    "能否取得 URL 取决于会员、地区、版权与曲目权限，不绕过这些条件。",
    "- 应用便捷方法 `song_urls()` 对 standard / higher / exhigh 使用旧版 songs_url 的 128/192/320 kbps "
    "请求，以匹配当前后台 MP3 解码器。完整 songs_url_v1 仍可通过 call 使用。播放器必须检查服务实际返回的 type、URL 和 freeTrialInfo。",
    "- 后台客户端 `persist_cookies=false`，每次请求重载 GUI 持有的 cookies.txt，且不会覆写登录/退出状态。",
    "- download_img/download_song 流式写入 `.part` 文件并原子重命名；只接受 HTTP(S)，保留 TLS 校验。",
    "- Rust 的解析模型转换替换为原始 JSON，由应用模型统一处理空值、旧新字段和不可播放原因。",
    "",
    "## 功能逻辑映射",
    "",
    "| 用户流程 | API 顺序 |",
    "|---|---|",
    "| 扫码登录 | login_qr_create → 展示 qr_url → 每数秒 login_qr_check → 803 后 login_status |",
    "| 我的收藏 | login_status → user_song_id_list / user_song_list / album_sublist / user_radio_sublist |",
    "| 打开歌单 | song_list_detail → 读取 trackIds → 分批 songs_detail（保证完整列表） |",
    "| 播放队列 | song_urls() → songs_url（有损 MP3）→ 检查 type、URL 与权益 → 播放；过期/下一曲前重新取得 URL |",
    "| 正在播放 | song_lyric → 根据播放毫秒匹配歌词；播放状态驱动黑胶旋转，暂停冻结角度 |",
    "| 喜欢歌曲 | like_v1 → 成功后刷新 user_song_id_list；失败保留旧 UI 状态 |",
    "| 发现与搜索 | banners / top_song_list / new_albums / toplist / search_* |",
    "| 私人 FM 与心动 | personal_fm / fm_trash / playmode_intelligence_list → song_urls() |",
    "",
    "## 重新生成",
    "",
    "```sh",
    "git clone --branch 2.2.0 --depth 1 https://github.com/gmg137/netease-cloud-music-api.git /tmp/ncm-api",
    "python3 tools/generate_api_catalog.py /tmp/ncm-api --gtk ../netease-cloud-music-gtk",
    "```",
    "",
]
(out / "API_CATALOG.md").write_text("\n".join(lines))
print(f"Wrote {len(entries)} API entries and {len(session)} session helpers")

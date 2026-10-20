# 网易云音乐 API 全量清单

本清单先于界面功能划分建立，目的是让服务能力、界面调用、测试范围可以逐项追踪。

来源：`netease-cloud-music-gtk` 2.5.4 的 `Cargo.toml` 锁定了 [netease-cloud-music-api 2.2.0](https://github.com/gmg137/netease-cloud-music-api/tree/2.2.0)，commit `73ab46e5351fd3c31b67709ee1b4b918205b9e3e`。接口来自该开源客户端的一手代码，并非网易公开且保证兼容的商业 API。

范围：**49 个服务/下载方法 + 4 个会话辅助方法**。所有服务方法均可通过 `ApiClient::call(method, arguments)` 调用；返回服务原始 JSON。机器可读版本为 [api_catalog.json](api_catalog.json)。

状态含义：表内为**代码实现范围**，不等于已用真实账户和 Switch 硬件验证。实时验证结果单独记录在 [API_VALIDATION.md](API_VALIDATION.md)。

## 接口矩阵

| 方法 | 用途 | C++ 参数（省略项采用默认值） | 协议 | 参考端点 |
|---|---|---|---|---|
| `fetch_xeapi_public_key` | 取得并缓存 XEAPI 动态公钥 | 无 | FORM+AES-256 | `https://interface.music.163.com/api/gorilla/anti/crawler/security/key/get` |
| `login` | 密码登录（邮箱或手机号） | username, password 或 password_md5 | WEAPI | `/weapi/login/cellphone`<br>`/weapi/login` |
| `login_cellphone` | 手机短信验证码登录 | phone, captcha, ctcode=86 | WEAPI | `/weapi/w/login/cellphone` |
| `captcha` | 发送登录短信验证码 | phone, ctcode=86 | WEAPI | `/weapi/sms/captcha/sent` |
| `login_qr_create` | 取得登录二维码 key 与扫码 URL | 无 | WEAPI | `/weapi/login/qrcode/unikey` |
| `login_qr_check` | 轮询扫码登录状态 | key | WEAPI | `/weapi/login/qrcode/client/login` |
| `login_status` | 当前账户、个人资料与登录状态 | 无 | WEAPI | `/api/nuser/account/get` |
| `logout` | 退出登录并清除本机会话 | 无 | WEAPI | `https://music.163.com/weapi/logout` |
| `daily_task` | 每日签到 | 无 | WEAPI | `/weapi/point/dailyTask` |
| `user_song_id_list` | 用户喜欢歌曲的 ID 列表 | uid | WEAPI | `/weapi/song/like/get` |
| `user_song_list` | 用户创建及收藏的歌单 | uid, offset=0, limit=30 | WEAPI | `/weapi/user/playlist` |
| `album_sublist` | 用户收藏专辑 | offset=0, limit=30 | WEAPI | `/weapi/album/sublist` |
| `user_cloud_disk` | 用户音乐云盘 | offset=0, limit=30 | WEAPI | `/weapi/v1/cloud/get` |
| `song_list_detail` | 歌单详情、tracks 与完整 trackIds | songlist_id (别名 id), offset=0, limit=1000, n=1000 | WEAPI | `/weapi/v6/playlist/detail` |
| `songs_detail` | 批量歌曲详情与权限信息 | ids:正整数数组，1–1000项 | WEAPI | `/weapi/v3/song/detail` |
| `songs_url` | 按码率取得播放 URL | ids, br=320000 | EAPI | `/api/song/enhance/player/url` |
| `songs_url_v1` | 按音质取得播放 URL、时效、格式与权限 | ids, level=standard, encodeType=mp3/flac | EAPI | `/api/song/enhance/player/url/v1` |
| `recommend_resource` | 每日推荐歌单 | 无 | WEAPI | `/weapi/v1/discovery/recommend/resource` |
| `recommend_songs` | 每日推荐歌曲 | 无 | WEAPI | `/api/v3/discovery/recommend/songs` |
| `personal_fm` | 私人 FM | 无 | WEAPI | `/weapi/v1/radio/get` |
| `like` | 旧版歌曲喜欢/取消喜欢 | songid (别名 id), like=true | WEAPI | `/weapi/radio/like` |
| `like_v1` | 新版歌曲喜欢/取消喜欢 | songid (别名 id), like=true | XEAPI | `/api/v1/radio/like` |
| `fm_trash` | 私人 FM 不喜欢 | songid (别名 id) | WEAPI | `/weapi/radio/trash/add` |
| `search` | 综合类型搜索 | keywords (别名 s), types/type=1, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `search_song` | 搜索歌曲 | keywords, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `search_singer` | 搜索歌手 | keywords, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `search_album` | 搜索专辑 | keywords, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `search_songlist` | 搜索歌单 | keywords, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `search_lyrics` | 按歌词搜索歌曲 | keywords, offset=0, limit=30 | WEAPI | `/weapi/search/get` |
| `singer_songs` | 歌手热门歌曲 | id | WEAPI | `/weapi/v1/artist/{}` |
| `singer_all_songs` | 歌手全部歌曲 | id, order=hot, offset=0, limit=30 | WEAPI | `/weapi/v1/artist/songs` |
| `new_albums` | 分地区新碟 | area=ALL, offset=0, limit=30 | WEAPI | `/weapi/album/new` |
| `album` | 专辑与曲目详情 | album_id (别名 id) | WEAPI | `/weapi/v1/album/{}` |
| `songlist_detail_dynamic` | 歌单收藏与动态状态 | songlist_id (别名 id) | WEAPI | `/weapi/playlist/detail/dynamic` |
| `album_detail_dynamic` | 专辑收藏与动态状态 | album_id (别名 id) | WEAPI | `/weapi/album/detail/dynamic` |
| `top_song_list` | 分类热门/最新歌单 | cat=全部, order=hot, offset=0, limit=30 | WEAPI | `/weapi/playlist/list` |
| `top_song_list_highquality` | 精品歌单与时间戳分页 | cat=全部, lasttime=0 (64位时间戳), limit=30 | WEAPI | `/api/playlist/highquality/list` |
| `toplist` | 全部排行榜 | 无 | WEAPI | `/api/toplist` |
| `top_songs` | 排行榜歌曲（歌单详情别名） | list_id (别名 id), offset=0, limit=1000, n=1000 | WEAPI | `/weapi/v6/playlist/detail` |
| `song_lyric` | 原文、翻译及罗马音歌词 | music_id (别名 id) | WEAPI | `/weapi/song/lyric` |
| `song_list_like` | 收藏/取消收藏歌单 | id, like=true | WEAPI | `/weapi/playlist/subscribe`<br>`/weapi/playlist/unsubscribe` |
| `album_like` | 收藏/取消收藏专辑 | id, like=true | WEAPI | `/api/album/sub`<br>`/api/album/unsub` |
| `homepage` | App 首页区块 | cursor=null | WEAPI | `/api/homepage/block/page` |
| `banners` | 首页轮播内容 | clientType=pc | WEAPI | `/weapi/v2/banner/get` |
| `download_img` | 下载 CDN 图片，可请求缩略尺寸 | url, path, width=300, high=300 | GET | `调用者提供的 HTTP(S) CDN URL` |
| `download_song` | 下载已授权 CDN 音频 | url, path | GET | `调用者提供的 HTTP(S) CDN URL` |
| `user_radio_sublist` | 用户订阅电台 | offset=0, limit=30 | WEAPI | `/weapi/djradio/get/subed` |
| `radio_program` | 电台节目列表 | rid (别名 id), offset=0, limit=30 | WEAPI | `/weapi/dj/program/byradio` |
| `playmode_intelligence_list` | 心动模式智能队列 | sid (别名 song_id), pid (别名 playlist_id), count=1 | WEAPI | `/weapi/playmode/intelligence/list` |

## 会话与加密

| Rust 辅助方法 | Switch C++ 对应 |
|---|---|
| `new` | ApiClient(ApiConfig) |
| `from_cookie_jar` | ApiConfig.cookie_file（libcurl Netscape Cookie 格式） |
| `cookie_jar` | 内部 libcurl cookie engine；flush_cookies()/clear_session() |
| `set_proxy` | ApiClient::set_proxy() / ApiConfig.proxy |

- WEAPI：随机 16 字节会话密钥、两层 AES-128-CBC/PKCS#7、RSA-1024 无填充公钥运算。
- EAPI：API 路径和请求体的 MD5 摘要、AES-128-ECB/PKCS#7；播放 URL 支持账户 cookie。
- XEAPI：动态公钥握手、HMAC-SHA256、X25519、HKDF 兼容派生、AES-GCM 与 AES-ECB；用于新版喜欢接口。
- 底层使用 libcurl、mbedTLS 2.28 和 zlib。TLS 验证保持启用；Switch 的 CA 文件由 ApiConfig.ca_bundle 指定。
- 会话使用 `cookies.txt` 保存。扫码授权返回的 Set-Cookie 由 libcurl 保存，后续请求自动携带；退出清除会话。cookie 是账户凭据，不应提交到仓库或上传日志。
- 扫码状态 800（失效）、801（等待）、802（已扫描）、803（授权完成）作为正常状态返回；HTTP/服务失败抛出带 code 的 ApiError。

## 与参考实现的明确差异

- 参考实现若把 WEAPI 加密体发送到 `/api/`，本项目对应使用 `/weapi/` 路径；EAPI/XEAPI 保留签名 `/api/` 路径。修复参考 logout 中重复拼接主机名的问题。
- songs_detail 的 `c` 使用标准 JSON 数组编码，不再额外插入反斜杠。
- login 默认接收明文 password 后在本地 MD5；已计算摘要可传 password_md5。不会把密码写入日志。
- user_cloud_disk 采用可分页默认 30 首，避免一次请求 10000 首占用 Switch 内存；循环请求直到不足 limit 即可。
- 精品歌单 lasttime 接受完整毫秒时间戳，不沿用参考实现的 u8 限制。
- 有损音质默认请求 MP3 方便后台解码，其他音质请求 FLAC；可传 encodeType 覆盖。音质 level 为 standard / higher / exhigh / lossless / hires / jyeffect / sky / jymaster。能否取得 URL 取决于会员、地区、版权与曲目权限，不绕过这些条件。
- 应用便捷方法 `song_urls()` 对 standard / higher / exhigh 使用旧版 songs_url 的 128/192/320 kbps 请求，以匹配当前后台 MP3 解码器。完整 songs_url_v1 仍可通过 call 使用。播放器必须检查服务实际返回的 type、URL 和 freeTrialInfo。
- 后台客户端 `persist_cookies=false`，每次请求重载 GUI 持有的 cookies.txt，且不会覆写登录/退出状态。
- download_img/download_song 流式写入 `.part` 文件并原子重命名；只接受 HTTP(S)，保留 TLS 校验。
- Rust 的解析模型转换替换为原始 JSON，由应用模型统一处理空值、旧新字段和不可播放原因。

## 功能逻辑映射

| 用户流程 | API 顺序 |
|---|---|
| 扫码登录 | login_qr_create → 展示 qr_url → 每数秒 login_qr_check → 803 后 login_status |
| 我的收藏 | login_status → user_song_id_list / user_song_list / album_sublist / user_radio_sublist |
| 打开歌单 | song_list_detail → 读取 trackIds → 分批 songs_detail（保证完整列表） |
| 播放队列 | song_urls() → songs_url（有损 MP3）→ 检查 type、URL 与权益 → 播放；过期/下一曲前重新取得 URL |
| 正在播放 | song_lyric → 根据播放毫秒匹配歌词；播放状态驱动黑胶旋转，暂停冻结角度 |
| 喜欢歌曲 | like_v1 → 成功后刷新 user_song_id_list；失败保留旧 UI 状态 |
| 发现与搜索 | banners / top_song_list / new_albums / toplist / search_* |
| 私人 FM 与心动 | personal_fm / fm_trash / playmode_intelligence_list → song_urls() |

## 重新生成

```sh
git clone --branch 2.2.0 --depth 1 https://github.com/gmg137/netease-cloud-music-api.git /tmp/ncm-api
python3 tools/generate_api_catalog.py /tmp/ncm-api --gtk ../netease-cloud-music-gtk
```

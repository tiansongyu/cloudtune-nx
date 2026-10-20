# API 验证记录

记录时间：2026-10-20 04:44 UTC。环境：Ubuntu 22.04 x86_64、libcurl 7.81、mbedTLS 2.28.10；Switch 交叉编译使用本地 Docker 镜像 `netease-switch/toolchain:local`。

项目现名 **CloudTune NX**。上面的镜像名称保留验证当时的实际记录；当前默认镜像为
`cloudtune-nx/toolchain:local`，最新构建步骤见 [BUILDING.md](BUILDING.md)。

后续二维码专用登录更新移除了 `login`、`login_cellphone`、`captcha` 三个方法，
当前保留 46 个服务 / 下载方法。下列 04:44 UTC 记录保留当时的历史范围；
新的离线测试另外确认这三个已删除的方法被拒绝，并验证多账号保存与切换。

## 已通过

- 49 个公开服务/下载方法完整列入清单；46 个可规划请求均进行离线构造测试，另 3 个是公钥握手和下载专用路径。
- WEAPI 双层 AES-CBC、RSA 无填充公钥运算和 EAPI 封包与 Python cryptography 独立生成的固定向量逐字节一致。
- XEAPI 随机封包使用 RFC 7748 固定测试公钥，由独立 Python cryptography 实现解密验证：X25519、HKDF 兼容派生、AES-GCM、两层 AES-ECB、随机掩码/旋转、HMAC-SHA256 均通过。
- 参数测试覆盖空 ID、非法音质、缺失必选参数、负分页、完整 64 位精品歌单时间戳、搜索别名以及歌曲详情 JSON 编码。
- `src/api.cpp` 和 `src/crypto.cpp` 均已使用 devkitA64 在 Docker 中实际交叉编译通过。加密使用 Switch `randomGet` 和 portlibs mbedTLS。
- Eden 实际运行发现 Horizon 不支持 rename 覆盖，已停止使用 libcurl COOKIEJAR 磁盘写入。应用改为导出 cookie，
  完成新文件后备份旧文件再替换，失败回滚；同一方案用于设置和下载。`ncm-storage-tests` 强制主机采用
  Horizon 的禁止覆盖语义，并注入替换及回滚失败，验证旧数据恢复、重复保存、会话重启、后台只读和退出持久化。
- 以下 **21 个不同方法** 实际通过 HTTPS 调用服务成功；未使用模拟返回。

| 实际方法 | 结果 |
|---|---|
| banners | code 200，轮播数据 |
| toplist | code 200，排行榜列表 |
| search_song | code 200，关键词搜索结果 |
| songs_detail | code 200，歌曲与权限详情 |
| songs_url | code 200，真实 MP3 URL |
| songs_url_v1 | code 200，`encodeType=mp3` 返回 MP3 |
| song_lyric | code 200，歌词 |
| album | code 200，专辑及曲目 |
| album_detail_dynamic | code 200，专辑动态 |
| singer_songs | code 200，歌手热门歌曲 |
| singer_all_songs | code 200，分页歌曲 |
| download_img | 真实封面下载并写入本地文件 |
| top_song_list | code 200，热门歌单 |
| song_list_detail | code 200，真实歌单详情 |
| songlist_detail_dynamic | code 200，歌单动态 |
| top_song_list_highquality | code 200，精品歌单 |
| new_albums | code 200，新碟列表 |
| homepage | code 200，首页区块 |
| fetch_xeapi_public_key | code 200，公钥握手并成功解密 |
| login_qr_create | code 200，合法二维码 key / URL |
| login_qr_check | code 801，正常等待扫码 |

测试曲目 ID `347230` 的匿名播放结果为 MP3、约 128 kbps、URL 时效 1200 秒；服务授予的是 **45 秒试听**，并返回 `freeTrialInfo`。这是实际账户权益限制，不能据此宣称整首歌曲或付费内容可完整播放。报告没有保存登录 cookie、二维码 key 或带签名的音频 URL。

## 用户登录后的只读回归 — 2026-10-20

用户在当前桌面的 Eden NRO 中完成登录，界面显示已登录状态。
随后通过相同 C++ `ApiClient` 读取该会话，使用宿主 libcurl 的证书及主机名验证，
并设置 `persist_cookies=false`，不覆盖正在运行的 NRO 会话。

以下 8 项均实际返回 `code=200`，且包含有效登录资料或非空列表：

| 方法 | 验证内容 |
| --- | --- |
| login_status | 登录身份有效 |
| user_song_id_list | 喜欢歌曲 ID 列表 |
| user_song_list | 创建及收藏的歌单 |
| album_sublist | 收藏专辑 |
| user_radio_sublist | 订阅电台 |
| user_cloud_disk | 云盘首批分页 |
| recommend_songs | 每日推荐歌曲 |
| personal_fm | 私人 FM 曲目 |

此轮不提交收藏、签到、短信或退出账号等写操作，也不打断用户正在操作的模拟器。
公开记录不包含账号标识、昵称、歌单名称、库大小或原始响应。脱敏计数仅保留在
被 Git 忽略的 `build/validation/auth-readonly-results.txt`。
这是实际会话的宿主 API 回归；各资料库页面在 NRO 中的完整交互仍需分别验收。

## 仍需账户和设备验证

以下场景尚未完成验证：

- 二维码全部中间状态、超时/取消处理，以及重启 NRO 后的真实会话恢复。
- 服务端退出接口的闭环；密码和短信登录已从当前应用移除，不再属于验收范围。
- 资料库各页面与官方客户端逐项比对，以及多页资料的完整浏览。
- 喜欢/取消喜欢、收藏/取消收藏、FM 不喜欢、签到等写操作。XEAPI 加密已独立验证并且公钥握手成功，但尚未对用户账户提交收藏操作。
- 每日推荐、私人 FM 在不同账户和权益下的行为，以及心动模式。
- Switch 上的真实网络/TLS/音频输出、锁屏、休眠、游戏后台并发与覆盖层交互。

## 复现

```sh
cmake --build build/desktop --target ncm-api-tests ncm-api-probe
ctest --test-dir build/desktop --output-on-failure
build/desktop/ncm-api-tests --fixture | python3 tests/verify_xeapi.py
build/desktop/ncm-api-probe
build/desktop/ncm-api-probe songs_detail '{"ids":[347230]}'
```

最后一项诊断命令会输出 API 原始 JSON。不要把包含凭据、二维码 key 或用户资料的诊断输出公开发布。`verify_xeapi.py` 的独立交叉验证需要 Python `cryptography`；C++ 固定向量测试不需要该 Python 依赖。

# CloudTune NX 发布流程

维护入口：[GitHub Actions](https://github.com/tiansongyu/cloudtune-nx/actions)、
[CI 工作流](https://github.com/tiansongyu/cloudtune-nx/actions/workflows/ci.yml)、
[Release 工作流](https://github.com/tiansongyu/cloudtune-nx/actions/workflows/release.yml)、
[版本下载](https://github.com/tiansongyu/cloudtune-nx/releases)。

本页说明自动化流程及发布规则。某次构建是否通过，以对应提交的 Actions
运行记录为准；已有本地、API 和模拟器测试见 [VALIDATION.md](VALIDATION.md)
及 [API_VALIDATION.md](API_VALIDATION.md)。CI 通过不代表实机游戏内 BGM 已验收。

## CI 如何运行

| 触发方式 | 行为 | 发布权限 |
| --- | --- | --- |
| Pull request | 验证变更的宿主测试、Switch 三组件构建与打包 | 不创建 Release |
| 推送 `main` | 对主分支执行同一 CI，保留可检查的产物和日志 | 不创建 Release |
| 手工运行 CI | 在 Actions 的 CI 页面选择 ref 后运行 | 不创建 Release |
| 手工运行 Release | 填写 `vX.Y.Z`；测试成功后创建缺失标签并上传五个实际附件 | 仅发布任务需要写权限 |
| 推送 `vX.Y.Z` 标签 | 校验版本，调用同一 CI；全部成功后发布同次运行的附件 | 仅发布任务需要写权限 |

`.github/workflows/ci.yml` 同时提供 `workflow_call` 入口。Release 工作流
先验证请求版本与 `CMakeLists.txt` 中 `project(CloudTuneNX VERSION X.Y.Z ...)`
完全匹配，再调用该入口，等待宿主和 Switch 任务全部成功。
CI 输出确切的 artifact ID，发布任务按该 ID 下载**本次运行**生成的包，
不另取主分支的新二进制，也不从另一轮
`workflow_run` 拼接未经对应校验的产物。

宿主验证执行 `./scripts/test-host.sh`，包含 CTest、独立 XEAPI
验证，以及生成的 MP3 / AAC / FLAC、节流 HTTPS、多账号存储与播放隔离测试。
随后验证独立覆盖层服务，并在单独构建目录中执行完整 UBSan 测试。
Switch 验证使用固定
devkitPro 基础镜像构建 NRO、后台 NSP 和覆盖层，并检查包格式、程序 ID
及配套 ELF。具体命令和依赖见 [BUILDING.md](BUILDING.md)。

自动测试使用生成的音频、临时本地 TLS 证书和匿名测试数据；不需要账号
cookie、手机验证码、网易云会员凭据、主机密钥或模拟器用户配置。

## 构建与发布目录

```text
build/
├── desktop/                         # 宿主客户端、测试程序
├── switch/
│   ├── CloudTune.nro
│   ├── CloudTune.elf
│   └── background/
│       ├── cloudtune-background.nsp
│       └── cloudtune-background.elf
├── overlay/
│   ├── CloudTune.ovl
│   ├── CloudTune.elf
│   ├── CloudTune.nacp
│   ├── CloudTune.map
│   └── obj/                         # 覆盖层中间文件
├── package/                         # 打包暂存区
└── validation/                      # 本地验证日志和生成夹具
dist/
├── CloudTuneNX-<version>-<timestamp>-sd.zip
├── CloudTuneNX-<version>-<timestamp>-symbols.zip
├── CloudTuneNX-<version>-<timestamp>-docs.zip
├── CloudTuneNX-<version>-<timestamp>-SHA256SUMS
└── release/
    ├── CloudTuneNX-<version>-sd.zip
    ├── CloudTuneNX-<version>-symbols.zip
    ├── CloudTuneNX-<version>-docs.zip
    ├── CloudTuneNX-<version>-manifest.json
    └── SHA256SUMS
```

`overlay/` 保持为源码目录。编译产物位于 `build/`；可分发的附件位于 `dist/`。
这些生成目录都不提交到 Git。两个打包模式共享同一组格式与匹配符号检查：

```sh
# 带时间戳的本地候选包，保留之前的候选文件
./scripts/package.sh

# GitHub Release 的固定版本附件
./scripts/package.sh --release
```

Release 模式的固定文件名便于用户按版本下载，不表示不同构建具有相同哈希。
`CloudTuneNX-<version>-manifest.json` 记录源提交和各文件的 SHA-256；`SHA256SUMS`
用于检查下载附件。符号包内的目录为 `symbols/app/`、`symbols/background/`
和 `symbols/overlay/`，防止应用与覆盖层的 `CloudTune.elf` 互相覆盖。

## 发布一个版本

### 在 GitHub 网页手动发布

1. 在准备发布的分支上更新 `CMakeLists.txt` 版本、README 下载入口及
   `docs/releases/vX.Y.Z.md`，提交并推送。版本标签输入不会自动修改应用版本。
2. 打开 [Actions → Release](https://github.com/tiansongyu/cloudtune-nx/actions/workflows/release.yml)，
   点击 **Run workflow**，选择准备好的分支（通常为 `main`）。
3. 在 `tag` 中填写新标签，例如下一版 `v0.2.2`，再点击 **Run workflow**。
   输入必须与所选分支的 CMake 版本及发布说明一致；`v0.2.1` 已发布，不可重复使用。
4. 等待版本校验、完整宿主 / UBSan 测试、Switch 编译和包校验通过。
   缺失的标签会在发布任务中创建到**本次运行所测试的提交**；
   已有标签必须指向同一提交，不会被移动。
5. 工作流上传并回读校验五个附件后公开 Release；运行 Summary 提供安装 ZIP 链接。

下载产物是 `CloudTuneNX-X.Y.Z-sd.zip`，包含 `CloudTune.nro`、
`exefs.nsp` 和 `CloudTune.ovl`，另有 symbols、docs、manifest 和 `SHA256SUMS`。
不需要手工上传二进制，也不需要额外配置个人访问令牌。
后续通过本工作流发布的版本均为正式 Release（`prerelease=false`），并标记为 Latest。
输入已发布版本或已有草稿会停止操作，不覆盖原附件。

手动运行以点击时所选 ref 的提交为准，运行期间分支后续更新不会改变构建来源；
本地可复用 CI 使用同一提交。相关机制见 GitHub 的
[手动事件说明](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#workflow_dispatch)
和[工作流复用说明](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows)。

### 推送标签自动发布

1. 在待发布提交中更新 `CMakeLists.txt` 的版本和
   `docs/releases/vX.Y.Z.md` 发布说明。标签必须是无前导零的 `vX.Y.Z`；
   例如版本 `0.2.2` 对应标签 `v0.2.2`。工作流同时检查覆盖层版本来源，
   并在编译后验证应用和覆盖层的 NACP 版本。
2. 确认主分支 CI 成功，检查对应提交、自动测试日志和包清单。
   GUI 或音频行为变化还需要相应的模拟器与实机记录，不能由 CI 代替。
3. 在该提交创建并推送版本标签。以下命令为示例，已有标签不重复创建：

   ```sh
   git switch main
   git pull --ff-only
   git tag -a v0.2.2 -m "CloudTune NX v0.2.2"
   git push origin v0.2.2
   ```

4. 查看 Release 工作流：版本检查 → 可复用 CI → 新建 Release 草稿并上传 →
   下载草稿附件、验证来源和哈希、逐字节比对 → 正式公开。
   宿主或 Switch 任一必需检查失败时不会进入发布任务。修复后按实际失败阶段重新运行，
   或在需要修改源代码时提交新版本。
5. 检查 Release 页面上的版本、发布说明、五个附件及源提交是否一致，
   下载 SD 包重新核对 SHA-256。手动发布与标签触发均发布为
   **正式 Release**，包括 `0.x.y` 版本。

已发布的版本标签与附件作为历史记录保留。不强制移动标签，不用同名新包替换
已发布的附件；源代码修复应提升版本并创建新标签。失败运行的日志保留用于排查，
不将后来运行的成功状态追溯到此前失败的构建。
上传或回读校验失败时，草稿保留供维护者检查；已有 Release 或草稿会使新一轮
发布停止，工作流不会自行删除、覆盖附件或清理失败草稿。

## 权限与凭据

- CI 的构建和测试只授予所需的读取权限；上传构建产物不需要仓库内容写权限。
- `contents: write` 仅用于标签发布或维护者手动发布的最终任务，包括创建缺失标签。
  不要把它扩展到 PR 的编译任务。
- 不用 `pull_request_target` 执行未审查的 PR 代码，不向 PR 提供发布凭据或
  用户账号秘密。工作流中明确声明 `GITHUB_TOKEN` 的最小权限。
- 上传范围限定为发布附件和经过筛选的验证日志；不上传整个工作目录、
  `.local/`、cookie、生成的 TLS 私钥、模拟器 profile 或音频缓存。
- 对 Actions 本身、基础镜像或依赖版本的更新，和应用代码一样进行评审。

工作流运行于 Ubuntu 22.04，引用的官方 Actions 固定到完整提交 SHA。
CI 日志保留 7 天，供发布使用的构建 artifact 保留 14 天；长期下载入口是
GitHub Release，不能依赖临时 CI artifact 作为版本归档。

可复用工作流只能保持或收窄调用方授予的 token 权限，不能自行扩大权限；
这一约束以及 PR 事件的信任差异见 GitHub 的
[工作流复用说明](https://docs.github.com/en/actions/reference/workflows-and-actions/reusing-workflow-configurations)
和 [pull_request_target 安全说明](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target)。

## 用户下载安装

在 [Releases](https://github.com/tiansongyu/cloudtune-nx/releases)
选择版本，下载同页的 `CloudTuneNX-<version>-sd.zip` 与 `SHA256SUMS`。
在下载目录执行：

```sh
sha256sum --check --ignore-missing SHA256SUMS
```

确认所下载 SD 包显示 `OK`，再将其解压到 SD 卡根目录。
`--ignore-missing` 仅跳过没有下载的符号包等附件，已下载文件哈希不符仍会失败。
符号包、文档包和 manifest 留在电脑上；GitHub 自动生成的 Source code
压缩包用于构建源码，不是可安装的 SD 包。

更新时使用同一版本的应用、后台服务和覆盖层。保留
`sd:/config/netease-switch/`，既有会话和设置继续使用该兼容目录；
`ncm:bg` 服务及程序 ID `42000000004E434D` 保持不变。
详细 SD 布局、重启与旧入口清理见 [INSTALLING.md](INSTALLING.md)。

# So sánh TermX 1.1 với các terminal phổ biến

Tài liệu này so sánh TermX 1.1.0 (bản thêm SFTP và port forwarding) với bảy phần mềm terminal
đang được dùng nhiều trên Windows: PuTTY, TeraTerm, MobaXterm, WindTerm, SecureCRT, Termius và
Windows Terminal. Mục tiêu là thấy rõ TermX đang ở đâu, mạnh ở điểm nào và còn thiếu gì, để
chọn việc cho các bản sau.

Thông tin về các phần mềm khác dựa trên tài liệu công khai và các bản phát hành gần đây tính đến
cuối năm 2026. Giá, giới hạn bản miễn phí và một số tính năng thay đổi theo thời gian, nên trước
khi trích dẫn ra ngoài cần kiểm tra lại trên trang chính thức của từng hãng. Phần về TermX được
kiểm chứng trực tiếp trên mã nguồn và bộ test trong repo này.

## Tóm tắt

TermX 1.1 nay đã có đủ bộ ba mà dân mạng và DevOps cần hằng ngày: terminal SSH ổn định, trình
duyệt SFTP dùng chung kết nối, và port forwarding (local, remote, SOCKS). Với bản này, TermX đứng
ngang PuTTY, TeraTerm và WindTerm ở các tính năng cốt lõi, và vượt các phần mềm đó ở trải nghiệm
serial (tự nhận cáp USB console) cùng lịch sử lệnh có gợi ý. Khoảng cách còn lại so với MobaXterm
và SecureCRT nằm ở các tính năng "nâng cao": jump host, X11, macro/scripting, và các giao thức
khác như RDP/VNC. TermX chỉ chạy trên Windows và không có đồng bộ đám mây, nên không cạnh tranh
trực tiếp với Termius ở mảng đa thiết bị.

## Bảng so sánh tính năng

Ký hiệu: ✅ có sẵn, ◐ có một phần hoặc cần công cụ đi kèm, ❌ không có.

### Kết nối và giao thức

| Tính năng | TermX 1.1 | PuTTY | TeraTerm | MobaXterm | WindTerm | SecureCRT | Termius | Windows Terminal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| SSH2 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ qua `ssh.exe` |
| Telnet / raw TCP | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ Telnet | ❌ |
| Serial (COM) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ chỉ một số nền tảng | ❌ |
| Tự nhận cáp USB console | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| Shell cục bộ (PowerShell, cmd, WSL) | ✅ | ❌ | ❌ | ✅ | ✅ | ◐ | ◐ | ✅ |
| RDP / VNC / FTP | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ |
| Mosh | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ | ◐ |
| Đa nền tảng | ❌ chỉ Windows | ◐ có bản Unix | ❌ | ❌ | ✅ | ✅ | ✅ cả di động | ❌ |

### SSH nâng cao

| Tính năng | TermX 1.1 | PuTTY | TeraTerm | MobaXterm | WindTerm | SecureCRT | Termius | Windows Terminal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Duyệt file SFTP có giao diện | ✅ **mới** | ❌ (PSFTP dòng lệnh) | ❌ (chỉ SCP) | ✅ | ✅ | ✅ (SecureFX đầy đủ hơn) | ✅ | ❌ |
| SFTP dùng chung kết nối với terminal | ✅ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ❌ |
| Tải lên/tải về cả thư mục, kéo thả | ✅ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ❌ |
| Port forwarding local / remote | ✅ **mới** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ qua `ssh.exe` |
| Dynamic forwarding (SOCKS) | ✅ **mới** (SOCKS 4, 4a, 5) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ qua `ssh.exe` |
| Bật/tắt tunnel khi đang chạy, xem lưu lượng | ✅ | ◐ | ❌ | ✅ | ◐ | ✅ | ✅ | ❌ |
| Nhập rule từ PuTTY / `~/.ssh/config` | ✅ | — | ❌ | ◐ | ❌ | ◐ | ◐ | ✅ (đọc config gốc) |
| Jump host / ProxyJump | ❌ | ✅ | ◐ | ✅ | ✅ | ✅ | ✅ | ✅ |
| X11 forwarding | ❌ | ✅ (cần X server) | ✅ (cần X server) | ✅ có X server sẵn | ✅ | ✅ | ❌ | ◐ |
| Agent forwarding | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Pageant / OpenSSH agent | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ | ✅ |
| Thuật toán an toàn mặc định, hỏi khi phải dùng SHA-1/CBC | ✅ | ◐ cảnh báo | ◐ | ◐ | ◐ | ✅ cấu hình được | ◐ | ✅ |
| Zmodem / Xmodem / Kermit | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ |

### Làm việc hằng ngày

| Tính năng | TermX 1.1 | PuTTY | TeraTerm | MobaXterm | WindTerm | SecureCRT | Termius | Windows Terminal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tab và chia khung | ✅ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Cây session có thư mục | ✅ | ◐ danh sách phẳng | ❌ | ✅ | ✅ | ✅ | ✅ | ◐ profile |
| Lịch sử lệnh có gợi ý khi gõ | ✅ (SQLite FTS5) | ❌ | ❌ | ◐ | ✅ | ❌ | ✅ | ❌ |
| Snippets / lệnh lưu sẵn | ✅ | ❌ | ❌ | ✅ | ◐ | ✅ button bar | ✅ | ❌ |
| Gửi lệnh tới nhiều phiên một lúc | ✅ | ❌ | ◐ | ✅ multi-exec | ✅ | ✅ | ◐ | ◐ |
| Macro / scripting | ❌ | ❌ | ✅ TTL | ✅ macro | ◐ | ✅ Python, VBScript | ❌ | ❌ |
| Command palette | ✅ | ❌ | ❌ | ❌ | ◐ | ❌ | ✅ | ✅ |
| Ghi log phiên, có dấu thời gian | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ | ❌ |
| Mã hoá mật khẩu đã lưu | ✅ DPAPI | ❌ không lưu | ◐ | ✅ master password | ✅ | ✅ | ✅ vault | — |
| Đồng bộ đám mây, chia sẻ cho nhóm | ❌ | ❌ | ❌ | ❌ | ❌ | ◐ | ✅ | ❌ |
| Bản portable | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ◐ |

### Bản quyền và giá

| | TermX | PuTTY | TeraTerm | MobaXterm | WindTerm | SecureCRT | Termius | Windows Terminal |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Giấy phép | GPL-3.0, mã nguồn mở | MIT, mã nguồn mở | BSD, mã nguồn mở | Bản Home miễn phí có giới hạn; bản Pro trả phí | Miễn phí, mở một phần mã | Thương mại | Freemium, thuê bao | MIT, mã nguồn mở |
| Giới hạn bản miễn phí | Không | Không | Không | Giới hạn số session, tunnel và macro | Không | Chỉ dùng thử | Nhiều tính năng nhóm và đồng bộ cần trả phí | Không |

## Đánh giá theo từng đối thủ

**PuTTY.** Là chuẩn mực về độ ổn định và nhẹ, nhưng không có tab, không có SFTP có giao diện, và
quản lý session rất thô. TermX 1.1 nay làm được mọi thứ người dùng PuTTY hay dùng (SSH, serial,
forwarding, Pageant), nhập luôn session và rule forwarding từ PuTTY, và thêm tab, SFTP, lịch sử
lệnh. Điểm PuTTY vẫn hơn: jump host, X11, agent forwarding, và chạy được trên nhiều nền tảng.
Người dùng PuTTY là nhóm dễ chuyển sang TermX nhất.

**TeraTerm.** Mạnh ở serial và đặc biệt là ngôn ngữ macro TTL mà nhiều kỹ sư mạng dùng để tự động
cấu hình thiết bị, cùng với Xmodem/Zmodem để nạp firmware. TermX hơn ở tab, SFTP, tự nhận cáp USB
và lịch sử lệnh, nhưng chưa thay thế được TeraTerm cho ai phụ thuộc vào macro hoặc truyền file qua
serial.

**MobaXterm.** Đối thủ gần nhất về định vị "một cửa sổ cho mọi thứ". MobaXterm có nhiều thứ hơn:
X server có sẵn, RDP, VNC, FTP, Mosh, macro, bộ lệnh Unix. Đổi lại, bản miễn phí bị giới hạn số
session và tunnel, giao diện nặng và nhiều chức năng ít dùng. TermX 1.1 đã có SFTP dùng chung kết
nối và quản lý tunnel trực quan giống MobaXterm, không giới hạn, và có các điểm riêng (tự nhận cáp
console, cảnh báo thuật toán cũ, lịch sử lệnh có gợi ý). Để cạnh tranh sát hơn cần jump host và
X11.

**WindTerm.** Gần TermX nhất về phong cách: nhanh, miễn phí, có lịch sử lệnh gợi ý (TermX lấy cảm
hứng từ đây), SFTP tích hợp và forwarding. WindTerm có lợi thế đa nền tảng, Zmodem và X11. TermX
có lợi thế mã nguồn mở hoàn toàn, nhịp phát triển đều, kiểm thử tự động với sshd thật và fuzzing,
tự nhận cáp USB, và bảng tunnel hiển thị số kết nối và lưu lượng.

**SecureCRT.** Lựa chọn của doanh nghiệp: scripting Python, button bar, quản lý session rất mạnh,
jump host, X11, Zmodem, và SecureFX cho truyền file chuyên sâu. TermX không cạnh tranh ở mảng tự
động hoá, nhưng miễn phí và đủ dùng cho phần lớn công việc hằng ngày. Thêm scripting là bước lớn
nhất nếu muốn kéo người dùng SecureCRT.

**Termius.** Mạnh ở đồng bộ đám mây, chia sẻ host cho nhóm và chạy được cả trên điện thoại. TermX
không đi theo hướng này (chỉ Windows, dữ liệu ở máy người dùng), và đó cũng là ưu điểm với những
nơi không muốn đưa thông tin đăng nhập lên đám mây. Termius không có serial đầy đủ, nên không phù
hợp với kỹ sư mạng làm việc với console thiết bị.

**Windows Terminal.** Terminal hiện đại, render nhanh, tương thích VT rất tốt, nhưng chỉ là trình
hiển thị: SSH và forwarding phải qua `ssh.exe`, không có session manager, không có SFTP, không có
serial. Hai phần mềm bổ sung cho nhau hơn là thay thế nhau; TermX phù hợp hơn cho người quản lý
nhiều thiết bị và cần serial.

## TermX 1.1 có gì mới

- **SFTP Browser** (Ctrl+Shift+O): mở trên chính kết nối SSH của tab đang chọn, không cần đăng nhập
  lại. Duyệt thư mục, tải lên/tải về file và cả thư mục (kéo thả được), đổi tên, xoá, đổi quyền,
  mở terminal tại thư mục (`cd`). Hàng đợi truyền có thanh tiến trình, tốc độ, nút huỷ. File tải
  về được ghi ra file tạm rồi mới đổi tên, nên huỷ hay lỗi giữa chừng không để lại file hỏng; huỷ
  khi tải lên sẽ xoá file dở trên máy chủ. Tên file từ máy chủ được làm sạch (chặn `..`, ký tự và
  tên thiết bị không hợp lệ trên Windows) để máy chủ không thể ghi file ra ngoài thư mục đã chọn.
- **Port Forwarding** (Ctrl+Shift+J): local (`-L`), remote (`-R`) và dynamic SOCKS 4/4a/5 (`-D`).
  Lưu rule cùng session, hoặc thêm và dừng tunnel khi đang chạy bằng cú pháp OpenSSH quen thuộc.
  Bảng tunnel hiện trạng thái, số kết nối và lưu lượng; có thể "Save with Session" một tunnel đang
  chạy. Quick Connect hiểu `ssh -L/-R/-D`, và rule được nhập từ PuTTY và `~/.ssh/config`. Cảnh báo
  khi mở cổng ra ngoài máy (không phải loopback).

## Debug và hardening trong bản này

- **Lỗi mất dữ liệu khi nhiều kênh dùng chung một kết nối.** libssh2 chỉ giữ được một gói tin gửi
  dở cho mỗi phiên. Khi terminal, SFTP và các tunnel cùng gửi, bản 1.11.0 có thể báo nhầm là đã gửi
  xong và làm mất dữ liệu, còn bản 1.11.1 có thể treo vĩnh viễn. TermX 1.1 đưa vào cơ chế "chủ sở
  hữu" để lệnh gửi dở luôn được hoàn tất trước khi kênh khác gửi tiếp, và tuần tự hoá các thao tác
  mở kênh. Keep-alive chỉ được gửi khi chắc chắn không chen vào gói tin khác.
- **Rò rỉ phiên SSH khi đóng kết nối** (có từ trước bản 1.1): ở chế độ non-blocking,
  `libssh2_session_free` có thể trả về EAGAIN và bỏ lại phiên. Đã sửa bằng cách đóng socket trước
  khi giải phóng; AddressSanitizer xác nhận không còn rò rỉ.
- **Kiểm thử tích hợp với OpenSSH thật** (`tst_sshintegration`): shell, SFTP (file, thư mục, huỷ),
  cả ba loại forwarding, lỗi forwarding, mọi thứ chạy đồng thời, và gọi API sau khi đã ngắt kết nối.
  Chạy sạch cả với ASan/UBSan. CI Linux nay cài `openssh-server` để chạy bộ test này.
- **Fuzzing bộ phân tích SOCKS** (`fuzz_socks`), vì đây là dữ liệu đến từ bất kỳ chương trình nào
  kết nối vào cổng SOCKS. 15 triệu lần chạy không có lỗi; đã đưa vào vòng fuzz của CI.
- **Giới hạn tài nguyên**: tối đa 256 tunnel đồng thời mỗi phiên, bộ đệm 256 KB mỗi chiều, gói
  SOCKS tối đa 1 KB, danh sách rule lưu tối đa 64; tunnel không đóng được sẽ bị bỏ sau 20 giây, và
  phiên bị kẹt quá 120 giây sẽ ngắt thay vì treo giao diện.

## Còn thiếu gì, và nên làm gì tiếp

Xếp theo mức độ người dùng hỏi tới và công sức ước tính:

1. **Jump host / ProxyJump.** Thiếu sót rõ nhất so với mọi đối thủ có SSH; hiện TermX chỉ cảnh báo
   khi nhập `~/.ssh/config` có ProxyJump. Có thể dựng trên `direct-tcpip` đã có cho forwarding.
2. **Agent forwarding.** Cần cho người dùng git và các máy trung gian; libssh2 hỗ trợ sẵn.
3. **X11 forwarding.** Cần một X server ngoài (VcXsrv, X410) như PuTTY; tích hợp X server như
   MobaXterm là việc lớn hơn nhiều.
4. **Macro / scripting.** Điều kiện để kéo người dùng TeraTerm và SecureCRT. Hướng hợp lý: một ngôn
   ngữ nhúng nhỏ (Lua hoặc JavaScript qua QJSEngine) với các lệnh `send`, `wait`, `expect`.
5. **Xmodem/Zmodem qua serial** để nạp firmware thiết bị mạng.
6. **SSH certificate, khoá FIDO (sk-ed25519)** và các tuỳ chọn xác thực doanh nghiệp.
7. **Đồng bộ session** (ví dụ qua thư mục OneDrive hoặc file xuất/nhập có mã hoá), đơn giản hơn
   nhiều so với một dịch vụ đám mây riêng.
8. **macOS/Linux.** Lõi đã build và test trên Linux, nhưng phần giao diện và các tính năng
   Windows-only (ConPTY, DPAPI, nhận cáp USB) cần làm lại.

## Kết luận

Với SFTP và port forwarding, TermX 1.1 đã đủ để thay PuTTY, TeraTerm (khi không cần macro) và phần
lớn cách dùng MobaXterm bản miễn phí cho công việc SSH và serial hằng ngày, mà không có giới hạn
bản quyền nào. Ba điểm khác biệt đáng giữ là trải nghiệm serial, lịch sử lệnh có gợi ý, và mức độ
kiểm thử (test với sshd thật, sanitizer, fuzzing). Ưu tiên tiếp theo nên là jump host và agent
forwarding, rồi tới scripting.

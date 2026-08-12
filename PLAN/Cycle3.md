
# AuroraOS Cycle 3
# Kernel/Userspace 鍒嗙涓庢湇鍔″寲闃舵

> 鐗堟湰锛?.0
> 椤圭洰锛欰uroraOS
> 鍐呮牳锛欽uly Kernel
> 闃舵锛欳ycle 3 - Services & Separation
> 鍓嶇疆锛欳ycle 2 - Microkernel Core
> 鐘舵€侊細瑙勫垝涓?

---

# 1. 闃舵姒傝堪

Cycle 3 鏄?AuroraOS 浠?澶у瀷 RTOS"杩涗竴姝ヨ蛋鍚戠湡姝ｅ井鍐呮牳鐨勯噸瑕侀樁娈点€?

Cycle 2 寤虹珛浜?IPC銆丆apability銆丼yscall 绛夊井鍐呮牳鏍稿績鏈哄埗銆侰ycle 3 鍦ㄦ鍩虹涓婂皢澶嶆潅鍔熻兘浠?Kernel 涓垎绂诲嚭鏉ワ紝浠?Service 褰㈠紡杩愯銆?

鏈樁娈电殑鏍稿績鐩爣锛?

> 灏?VFS銆丯etwork銆丗irewall銆丼canner 绛夊鏉傜郴缁熶粠 Kernel 涓殧绂伙紝浣滀负鐙珛鐨?User Space Service 杩愯锛岄€氳繃 IPC 涓?Kernel 鍜屽叾浠?Task 閫氫俊銆?

---

# 2. 闃舵鐩爣

鐩爣鏋舵瀯锛?

```
                July Kernel
                     鈹?
              Syscall / IPC
                     鈹?
       鈹屸攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹尖攢鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹?
       鈫?            鈫?            鈫?
   VFS Service   Network Service  Device Service
       鈹?            鈹?            鈹?
       鈫?            鈫?            鈫?
   Filesystems       lwIP         Drivers
```

Kernel 涓嶅啀鐩存帴鍖呭惈澶ч噺楂樼骇鏈嶅姟閫昏緫銆?

---

# 3. 鏃堕棿瑙勫垝

棰勮鍛ㄦ湡锛?

```
5 锝?8涓湀
```

---

# 4. VFS Service

## 4.1 鐩爣

灏嗘枃浠剁郴缁熶粠 Kernel 涓垎绂讳负鐙珛 Service锛?

```
VFS Service
鈹溾攢鈹€ VNode
鈹溾攢鈹€ File
鈹溾攢鈹€ RamFS
鈹溾攢鈹€ ProcFS
鈹溾攢鈹€ LittleFS
鈹斺攢鈹€ PhotonCache
```

## 4.2 鏋舵瀯

```
Application
    鈫?
VFS API (IPC)
    鈫?
VFS Service
    鈫?
Filesystem Driver
    鈫?
Storage Driver
```

## 4.3 Kernel 鑱岃矗

July 鍙彁渚涳細

- IPC 閫氶亾
- Memory 鍏变韩
- Capability 鎺堟潈
- Task 绠＄悊

VFS 浣滀负 Service 杩愯锛屼笉杩涘叆 Kernel銆?

---

# 5. Network Service

## 5.1 鐩爣

寤虹珛鐙珛缃戠粶鏈嶅姟锛?

```
Network Service
鈹溾攢鈹€ lwIP Stack
鈹溾攢鈹€ Ethernet Driver Interface
鈹溾攢鈹€ WiFi Driver Interface
鈹溾攢鈹€ BLE Stack
鈹溾攢鈹€ Firewall Engine
鈹溾攢鈹€ Packet Capture
鈹溾攢鈹€ Network Scanner
鈹溾攢鈹€ IDS
鈹斺攢鈹€ Distributed SoftBus
```

## 5.2 鏋舵瀯

```
Application
    鈫?
Socket API (IPC)
    鈫?
Network Service
    鈫?
lwIP
    鈫?
Network Driver
```

## 5.3 鍒嗙鍘熷垯

```
July Kernel
 鈹?
 鈹斺攢鈹€ IPC (only)
       鈫?
Network Service
       鈫?
      lwIP
       鈫?
    Driver
```

缃戠粶鍗忚鏍堜笉杩涘叆 Kernel銆?

---

# 6. NetworkScanner 閲嶆瀯

## 6.1 褰撳墠闂

Scanner 鏄噸鐐规不鐞嗙殑 God Object锛屾墍鏈夋壂鎻忕被鍨嬭€﹀悎鍦ㄤ竴璧枫€?

## 6.2 鐩爣鏋舵瀯

```
NetworkScanner
鈹溾攢鈹€ Engine
鈹溾攢鈹€ Worker
鈹溾攢鈹€ Queue
鈹溾攢鈹€ Handler Registry
鈹?
鈹溾攢鈹€ TCP Handler
鈹溾攢鈹€ UDP Handler
鈹溾攢鈹€ ARP Handler
鈹溾攢鈹€ ICMP Handler
鈹?
鈹溾攢鈹€ Service Detection
鈹斺攢鈹€ Vulnerability Detection
```

## 6.3 鎵╁睍鏂瑰紡

鏂板鎵弿鏂瑰紡鏃讹細

```
鏂板 Handler
```

鑰屼笉鏄笉鏂慨鏀癸細

```
ScanEngine.cpp
```

閲囩敤绛栫暐/Handler 鎺ュ彛妯″紡锛屼繚鎸佹墿灞曟€с€?

---

# 7. Firewall 2.0

## 7.1 鐩爣

褰㈡垚鐙珛 Firewall Service锛?

```
Firewall Service
鈹溾攢鈹€ Rule Engine
鈹溾攢鈹€ Connection Tracking
鈹溾攢鈹€ Rate Limiting
鈹溾攢鈹€ Policy Manager
鈹斺攢鈹€ Audit Log
```

## 7.2 瀹夊叏绛栫暐鍒嗙

```
Network Packet
    鈫?
Firewall Service
    鈫?
Policy Engine
    鈫?
Rule Matching
    鈫?
Accept / Drop / Log
```

瀹夊叏绛栫暐涓庣綉缁滃崗璁疄鐜板垎绂汇€?

## 7.3 鑳藉姏鐩爣

- 瑙勫垯寮曟搸锛堝尮閰嶃€佷紭鍏堢骇锛?
- 杩炴帴杩借釜
- 閫熺巼闄愬埗
- 瀹¤鏃ュ織
- 鍔ㄦ€佽鍒欐洿鏂?

---

# 8. Driver / HAL 浣撶郴閲嶆瀯

## 8.1 鐩爣

寤虹珛娓呮櫚鐨勫垎灞傞┍鍔ㄤ綋绯伙細

```
Application
 鈫?
Service
 鈫?
Driver API
 鈫?
HAL
 鈫?
Architecture
 鈫?
Hardware
```

## 8.2 绀轰緥

```
Display Service
      鈫?
Display Driver
      鈫?
SPI HAL
      鈫?
Cortex-M SPI
      鈫?
Hardware
```

## 8.3 绂佹浜嬮」

绂佹锛?

```
Application
 鈫?
鐩存帴鎿嶄綔瀵勫瓨鍣?
```

鎵€鏈夌‖浠惰闂繀椤婚€氳繃 HAL 鈫?Driver 鈫?Service 灞傛銆?

---

# 9. Board 鏀寔浣撶郴鏍囧噯鍖?

## 9.1 鐩爣

```
boards/
鈹溾攢鈹€ lm3s6965/
鈹溾攢鈹€ nucleo_l031k6/
鈹溾攢鈹€ miband8/
鈹斺攢鈹€ qemu_rv32/
```

## 9.2 姣忎釜 Board 搴斿寘鍚?

- memory map
- clock configuration
- peripheral mapping
- linker configuration
- HAL configuration
- board initialization

## 9.3 CMake 瑙勮寖

閬垮厤灏嗗ぇ閲?Board 鍒ゆ柇鍐欒繘鏍圭洰褰?CMake銆?

Board 閰嶇疆搴旀斁鍦?`boards/` 鐩綍涓嬨€?

---

# 10. 瀹夊叏鏈嶅姟

## 10.1 Security Monitor Service

```
Security Monitor
鈹溾攢鈹€ Syscall Audit
鈹溾攢鈹€ Capability Audit
鈹溾攢鈹€ IPC Monitor
鈹溾攢鈹€ Resource Usage Tracking
鈹斺攢鈹€ Alert System
```

## 10.2 Secure Boot Service

```
Secure Boot Service
鈹溾攢鈹€ Signature Verification
鈹溾攢鈹€ Firmware Metadata
鈹溾攢鈹€ A/B Partition Management
鈹溾攢鈹€ Rollback Protection
鈹斺攢鈹€ OTA Update Manager
```

---

# 11. 娴嬭瘯浣撶郴

## 11.1 Service Test

楠岃瘉锛?

- VFS Service 鏂囦欢鎿嶄綔
- Network Service 閫氫俊
- Firewall 瑙勫垯鍖归厤
- Scanner 鎵弿娴佺▼

## 11.2 Integration Test

楠岃瘉锛?

```
Application
    鈫?IPC
VFS Service
    鈫?IPC
Storage Driver
```

绔埌绔湇鍔¤皟鐢ㄦ祦绋嬨€?

## 11.3 Security Test

楠岃瘉锛?

- Service 闅旂
- Capability 杈圭晫
- 璧勬簮闄愬埗
- 閿欒浼犳挱

---

# 12. 寮€鍙戦噷绋嬬

# Milestone 1
## VFS 鏈嶅姟鍖?

浠诲姟锛?

- [x] VFS Service 鐙珛杩涚▼
- [x] IPC 鏂囦欢鎿嶄綔鎺ュ彛
- [x] RamFS / ProcFS 杩佺Щ
- [x] LittleFS 閫傞厤

瀹屾垚锛?

```
鏂囦欢鎿嶄綔閫氳繃 IPC 瀹屾垚锛孷FS 涓嶅湪 Kernel 涓?
```

---

# Milestone 2
## Network 鏈嶅姟鍖?

浠诲姟锛?

- [x] Network Service 鐙珛杩涚▼
- [x] Socket IPC 鎺ュ彛
- [x] lwIP 闆嗘垚
- [x] 缃戠粶椹卞姩鎺ュ彛

瀹屾垚锛?

```
缃戠粶閫氫俊閫氳繃 Network Service 浠ｇ悊
```

---

# Milestone 3
## Driver/HAL 閲嶆瀯

浠诲姟锛?

- [x] 缁熶竴 Driver API
- [x] HAL 鎺ュ彛鏍囧噯鍖?
- [x] 鏄剧ず椹卞姩鍒嗗眰
- [x] Board 閰嶇疆鏍囧噯鍖?

瀹屾垚锛?

```
纭欢璁块棶灞傛娓呮櫚锛屾棤璺ㄥ眰璋冪敤
```

---

# Milestone 4
## Scanner 閲嶆瀯

浠诲姟锛?

- [x] Handler 鎺ュ彛璁捐
- [x] TCP/UDP/ARP/ICMP Handler 鎷嗗垎
- [x] Engine 涓?Worker 鍒嗙
- [x] Service Detection 妯″潡鍖?

瀹屾垚锛?

```
Scanner 涓嶅啀鏄竴涓?God Object
```

---

# Milestone 5
## Firewall 2.0

浠诲姟锛?

- 鐙珛 Firewall Service
- 瑙勫垯寮曟搸
- 杩炴帴杩借釜
- 瀹¤鏃ュ織

瀹屾垚锛?

```
缃戠粶瀹夊叏绛栫暐鐙珛绠＄悊
```

---

# 13. 瀹屾垚鏍囧噯

Cycle 3 瀹屾垚鍚庯細

```
鉁?VFS 浣滀负鐙珛 Service 杩愯
鉁?Network 浣滀负鐙珛 Service 杩愯
鉁?Firewall 浣滀负鐙珛 Service 杩愯
鉁?Scanner 瀹屾垚妯″潡鍖栭噸鏋?
鉁?Driver/HAL 灞傛娓呮櫚
鉁?Board 閰嶇疆鏍囧噯鍖?
鉁?Service 涔嬮棿閫氳繃 IPC 閫氫俊
鉁?Kernel 涓嶅啀鍖呭惈楂樼骇鏈嶅姟閫昏緫
```

---

# 14. 涓嬩竴闃舵

杩涘叆锛?

```
Cycle 4
Runtime & Platform
```

閲嶇偣锛?

```
Aurora Runtime
App Model
UI Service
Sensor Framework
Power Management
```

---

# 15. 鏈€缁堢洰鏍?

Cycle 3 鐨勭洰鏍囨槸楠岃瘉寰唴鏍告灦鏋勭殑鍏抽敭鍋囪锛?

```
澶嶆潅鍔熻兘鍙互涓斿簲璇ュ湪 Kernel 涔嬪杩愯銆?
```

閫氳繃灏?VFS銆丯etwork銆丗irewall 绛夋湇鍔″寲锛岃瘉鏄?July Kernel 鐨?IPC/Capability 鏈哄埗瓒冲鏀拺瀹為檯绯荤粺闇€姹傦紝鍚屾椂淇濇寔 Kernel 鑷韩鐨勫皬鍨嬪寲鍜屽畨鍏ㄦ€с€?

---

# AuroraOS 鏍稿績鐞嗗康

> 鑳芥斁鍒?User Space锛屽氨涓嶈鏀捐繘 July銆?

Kernel 瓒婂皬锛岃秺瀹规槗楠岃瘉銆佽秺瀹夊叏銆佽秺鍙淮鎶ゃ€?




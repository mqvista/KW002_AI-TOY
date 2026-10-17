# 固件逆向脚本

分析 T5AI（BK7236）整片 flash dump 用。依赖：Python 3 + `capstone`（`pip install capstone`）。

| 脚本 | 用法 | 作用 |
|---|---|---|
| `regions.py` | `python regions.py dump.bin` | 按 4K 扇区判断哪些区域带 34 字节块 CRC16（多项式 0x8005，初值 0xFFFF） |
| `strip.py` | `python strip.py dump.bin code.bin strings_code.txt strings_data.txt` | 去掉代码区（0~0x31B000）的 CRC 得到 `code.bin`，导出代码区/数据区字符串 |
| `dis.py` | `python dis.py code.bin none 0x0216E520 0x0216E660` | Thumb 反汇编指定地址区间，自动标注 PC 相对加载的常量和字符串 |
| `xref.py` | `python xref.py code.bin 0x02010000-0x021F0000 0x216E521` | 查找调用指定函数（BL/BLX/B.W）或指向它的函数指针 |
| `callers.py` | `python callers.py code.bin 0x20f7f5d=gpio_init,0x20f7ff9=gpio_write` | 列出调用指定函数的位置以及调用前约 14 条指令（用来看参数） |

逻辑地址 = `0x02000000 + 去 CRC 后的偏移`。分析结论见 `docs/firmware_io_analysis.md`。

import io
import re
import sys

V3_IR = r"C:\Users\xiao\AppData\Local\Temp\modelprobe3\asr_v3.ll"
V4_IR = r"C:\Users\xiao\AppData\Local\Temp\modelprobe\asr_v4.ll"
OUT_IR = r"C:\Users\xiao\AppData\Local\Temp\modelprobe\asr_v4_edited.ll"


def read(path):
    return io.open(path, encoding="utf-8", errors="replace", newline=None).read()


v3 = read(V3_IR)
v4 = read(V4_IR)

# 1) 提取 V3 的小艾 grammar 常量完整行
m = re.search(r"@global_kws_lp_grammar = internal constant \[12560 x i8\].*", v3)
if not m:
    print("ERROR: V3 grammar definition not found")
    sys.exit(1)
v3_line = m.group(0).rstrip("\r\n")
# 替换符号名，作为新增的小艾 grammar
ai_line = v3_line.replace(
    "@global_kws_lp_grammar = internal constant",
    "@kws_grammar_ai = internal constant",
    1,
)

# 2) 在 V4 原 grammar 定义行之后插入小艾 grammar 定义
anchor = "@global_kws_lp_grammar = internal constant [14932 x i8]"
anchor_pos = v4.index(anchor)
line_end = v4.index("\n", anchor_pos)
insert = ai_line + "\n"
v4 = v4[: line_end + 1] + insert + v4[line_end + 1 :]

# 3) 在全局区加入测试标志外部声明（挂在 g_kws_share_mem 前面）
v4 = v4.replace(
    "@g_kws_share_mem",
    "@g_kws_test_mode = external global i8\n@g_kws_share_mem",
    1,
)

# 4) 替换初始化调用：按测试标志选择 grammar
call_marker = "%3 = tail call i8* @UalOFAInitializeFromBuffer"
call_pos = v4.index(call_marker)
call_end = v4.index("\n", call_pos)
new_block = (
    '%jin.ptr = getelementptr inbounds [14932 x i8], [14932 x i8]* @global_kws_lp_grammar, i32 0, i32 0\n'
    '%ai.ptr = getelementptr inbounds [12560 x i8], [12560 x i8]* @kws_grammar_ai, i32 0, i32 0\n'
    '%test.flag = load i8, i8* @g_kws_test_mode, align 1\n'
    '%test.on = icmp ne i8 %test.flag, 0\n'
    '%grammar.ptr = select i1 %test.on, i8* %ai.ptr, i8* %jin.ptr\n'
    '%3 = tail call i8* @UalOFAInitializeFromBuffer(i8* getelementptr inbounds '
    '([541848 x i8], [541848 x i8]* @global_kws_lp_acoutstic_model, i32 0, i32 0), '
    'i8* %grammar.ptr) #5, !dbg !467\n'
)
v4 = v4[:call_pos] + new_block + v4[call_end + 1 :]

io.open(OUT_IR, "w", encoding="utf-8", newline="\n").write(v4)
print("surgery OK, output size =", len(v4))

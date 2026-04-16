import re
import time
import asyncio
from pathlib import Path
from bleak import BleakScanner, BleakClient
from bleak.backends.characteristic import BleakGATTCharacteristic
from textual.app import App, ComposeResult, Notify
from textual.containers import ScrollableContainer, Horizontal
from textual.widgets import (
    Select,
    DataTable,
    Log,
    Input,
    Collapsible,
    Header,
    Footer,
    Switch,
    Button,
    Static,
    Label,
    RadioButton,
    RadioSet,
    TabbedContent,
    TabPane,
    LoadingIndicator,
)

# 显式定义字符串
DEVICE_NAME = "PowerRune24"
version = "v1.1.0"
UUID_Serv_Config = "00001827-0000-1000-8000-00805f9b34fb"
UUID_Serv_Operation = "00001828-0000-1000-8000-00805f9b34fb"
UUID_Char_URL = "00002aa6-0000-1000-8000-00805f9b34fb"
UUID_Char_SSID = "00002ac3-0000-1000-8000-00805f9b34fb"
UUID_Char_PSK = "00002a3e-0000-1000-8000-00805f9b34fb"
UUID_Char_AutoOTA = "00002ac5-0000-1000-8000-00805f9b34fb"
UUID_Char_Brightness_Armour = "00002a0d-0000-1000-8000-00805f9b34fb"
UUID_Char_Brightness_Arm = "00002a01-0000-1000-8000-00805f9b34fb"
UUID_Char_Brightness_RLogo = "00002a9b-0000-1000-8000-00805f9b34fb"
UUID_Char_Brightness_Matrix = "00002a9c-0000-1000-8000-00805f9b34fb"
UUID_Char_PID = "00002a66-0000-1000-8000-00805f9b34fb"
UUID_Char_Reset_Armour_ID = "00002b1f-0000-1000-8000-00805f9b34fb"
UUID_Char_RUN = "00002a65-0000-1000-8000-00805f9b34fb"
UUID_Char_Score = "00002a69-0000-1000-8000-00805f9b34fb"
UUID_Char_Unlock = "00002a3b-0000-1000-8000-00805f9b34fb"
UUID_Char_Stop = "00002ac8-0000-1000-8000-00805f9b34fb"
UUID_Char_OTA = "00002a9f-0000-1000-8000-00805f9b34fb"


class PowerRune24_Operations(Static):
    """A widget to display the available operations and attributes of PowerRune."""

    global client

    RUN_MODE_BIG = 0
    RUN_MODE_SMALL = 1
    RUN_MODE_SINGLE_TEST = 2
    RUN_MODE_SINGLE_SCORE_TEST = 8
    RUN_MODE_AUTO_SUCCESS = 9
    RUN_MODE_ALL_TARGET_READY = 3
    RUN_MODE_ALL_SUCCESS_STATIC = 4
    RUN_MODE_SMALL_4_HIT_1_READY_TEST = 5
    RUN_MODE_BIG_PROGRESS_2_READY_TEST = 7

    def compose(self) -> ComposeResult:
        with Horizontal(id="operation_buttons"):
            yield Button("▶启动", id="run_toggle", variant="success")
            yield Label("未运行", id="state")
        with Horizontal(id="feature_buttons"):
            yield Button("功能1 全靶待击打", id="func_all_target_ready", variant="primary")
            yield Button("功能2 全面成功常亮", id="func_all_success_static", variant="warning")
        # 得分
        with Collapsible(title="得分", collapsed=False):
            yield DataTable(id="score")
        with Collapsible(title="启动参数", collapsed=False):
            with Static(id="start_params"):
                yield Label("颜色方")
                with RadioSet(id="color"):
                    yield RadioButton("红方", value=True)
                    yield RadioButton("蓝方")

                yield Label("启动模式")
                with RadioSet(id="mode"):
                    yield RadioButton("大符模式", value=True)
                    yield RadioButton("小符模式")
                    yield RadioButton("单叶片测试")
                    yield RadioButton("单板测分测试")
                    yield RadioButton("自动成功模式（无视击打）")
                    yield RadioButton("小符调试（进度+待击打）")
                    yield RadioButton("大符调试（进度+双待击打）")

                yield Label("通用测试叶片")
                yield Select(
                    ((str(i), str(i)) for i in range(1, 6)),
                    id="test_leaf",
                    value="1",
                )

                yield Label("调试参数")
                with Horizontal(id="debug_params_row"):
                    with Static(id="small_debug_params"):
                        yield Label("小符已激活数(0~4)")
                        yield Select(
                            ((str(i), str(i)) for i in range(0, 5)),
                            id="small_progress",
                            value="0",
                        )
                        yield Label("小符待击打叶片")
                        yield Select(
                            ((str(i), str(i)) for i in range(1, 6)),
                            id="small_ready_leaf",
                            value="1",
                        )
                    with Static(id="big_debug_params"):
                        yield Label("大符进度档位(1~5)")
                        yield Select(
                            ((str(i), str(i)) for i in range(1, 6)),
                            id="big_progress",
                            value="1",
                        )
                        yield Label("大符待击打A")
                        yield Select(
                            ((str(i), str(i)) for i in range(1, 6)),
                            id="big_ready_leaf_a",
                            value="1",
                        )
                        yield Label("大符待击打B")
                        yield Select(
                            ((str(i), str(i)) for i in range(1, 6)),
                            id="big_ready_leaf_b",
                            value="2",
                        )

                yield Label("循环")
                yield Switch(value=True, id="loop")
                yield Label("方向")
                with RadioSet(id="direction"):
                    yield RadioButton("顺时针", value=True)
                    yield RadioButton("逆时针")
                yield Button("清空得分记录", id="clear", variant="error")

    def mode_text(self, mode_value: int) -> str:
        if mode_value == self.RUN_MODE_BIG:
            return "大符模式"
        if mode_value == self.RUN_MODE_SMALL:
            return "小符模式"
        if mode_value == self.RUN_MODE_SINGLE_TEST:
            return "单叶片测试"
        if mode_value == self.RUN_MODE_SINGLE_SCORE_TEST:
            return "单板测分测试"
        if mode_value == self.RUN_MODE_AUTO_SUCCESS:
            return "自动成功模式"
        if mode_value == self.RUN_MODE_ALL_TARGET_READY:
            return "功能1全靶待击打"
        if mode_value == self.RUN_MODE_ALL_SUCCESS_STATIC:
            return "功能2全面成功常亮"
        if mode_value == self.RUN_MODE_SMALL_4_HIT_1_READY_TEST:
            return "小符调试（进度+待击打）"
        if mode_value == self.RUN_MODE_BIG_PROGRESS_2_READY_TEST:
            return "大符调试（进度+双待击打）"
        return f"未知模式({mode_value})"

    def selected_mode_value(self) -> int:
        mode_index = self.query_one("#mode", RadioSet).pressed_index
        mode_value = self.mode_value_from_index(mode_index)
        if mode_value is None:
            return self.RUN_MODE_BIG
        return mode_value

    def mode_value_from_index(self, mode_index):
        mode_map = [
            self.RUN_MODE_BIG,
            self.RUN_MODE_SMALL,
            self.RUN_MODE_SINGLE_TEST,
            self.RUN_MODE_SINGLE_SCORE_TEST,
            self.RUN_MODE_AUTO_SUCCESS,
            self.RUN_MODE_SMALL_4_HIT_1_READY_TEST,
            self.RUN_MODE_BIG_PROGRESS_2_READY_TEST,
        ]
        if mode_index is None or mode_index < 0 or mode_index >= len(mode_map):
            return None
        return mode_map[mode_index]

    def mode_text_from_log_tag(self, mode_tag: str) -> str:
        mode_text_map = {
            "SingleTest": "单叶片测试",
            "SingleScoreTest": "单板测分测试",
            "Small4Hit1ReadyTest": "小符调试（进度+待击打）",
            "BigProgress2ReadyTest": "大符调试（进度+双待击打）",
        }
        return mode_text_map.get(mode_tag, mode_tag)

    def set_running_state(self, running: bool) -> None:
        self.running = running
        run_button = self.query_one("#run_toggle", Button)
        if running:
            run_button.label = "⏸暂停"
            run_button.variant = "warning"
            return
        run_button.label = "▶启动"
        run_button.variant = "success"

    def BLE_notify_handler(self, sender, data):
        # byte array->string
        data = data.replace(b"\x00", b"").decode("utf-8")
        # 更新log
        log.write_line("[BLE] %s" % data)
        self.notify(data, title="大符", severity="information")
        if data == "PowerRune Activation Failed" and self.running:
            self.update_score(0)
        # 正则表达式提取，目标为"[Score: %d]PowerRune Activated Successfully", score，不定长
        if re.match(r"\[Score: \d+\]PowerRune Activated Successfully", data):
            score = int(re.findall(r"\d+", data)[0])
            asyncio.create_task(self.sync_latest_score_from_device(score))
        hit_match = re.match(
            r"\[(?P<mode>[^\]]+)\]\s+Armour\s+(?P<armour>\d+)\s+hit\s+score\s+\+(?P<score>\d+)",
            data,
        )
        if hit_match:
            self.update_score(
                int(hit_match.group("score")),
                mode_name=self.mode_text_from_log_tag(hit_match.group("mode")),
                armour_id=int(hit_match.group("armour")),
            )
        if data == "PowerRune Run Complete":
            self.set_running_state(False)
            self.query_one("#state", Label).update("未运行")

    async def ensure_run_notify(self) -> None:
        if not self.first_notify_enabled:
            self.first_notify_enabled = True
            await client.start_notify(UUID_Char_RUN, self.BLE_notify_handler)

    async def sync_latest_score_from_device(self, fallback_score: int | None = None) -> None:
        if client is None or not connected:
            if fallback_score is not None:
                self.update_score(fallback_score)
            return

        latest_score = None
        try:
            score_data = await client.read_gatt_char(UUID_Char_Score)
            if score_data:
                latest_score = int(score_data[0])
                log.write_line("[Info] GPA latest score: %s" % list(score_data[:10]))
        except Exception as exc:
            log.write_line(f"[Warn] 读取分数特征失败: {exc}")

        if latest_score is None:
            latest_score = fallback_score

        if latest_score is not None:
            self.update_score(latest_score)

    def build_state_text(self) -> str:
        base_text = (
            "颜色:%s %s 循环%s 方向%s 叶片%d"
            % (
                "红方" if not self.start_params[0] else "蓝方",
                self.mode_text(self.start_params[1]),
                "✓" if self.start_params[2] else "✕",
                "↻" if not self.start_params[3] else "↺",
                self.start_params[4],
            )
        )
        if self.start_params[1] == self.RUN_MODE_SMALL_4_HIT_1_READY_TEST:
            return "%s 小符进度:%d/4 待击打:%d" % (
                base_text,
                self.start_params[7],
                self.start_params[4],
            )
        if self.start_params[1] == self.RUN_MODE_BIG_PROGRESS_2_READY_TEST:
            return "%s 大符待击打:%d,%d" % (
                base_text,
                self.start_params[5],
                self.start_params[6],
            )
        return base_text

    async def send_run(
        self,
        mode_value: int,
        loop_enabled: bool,
        direction_value: int,
        test_leaf: int,
        big_ready_a: int,
        big_ready_b: int,
        small_progress: int,
    ) -> None:
        await self.ensure_run_notify()
        self.start_params = (
            self.query_one("#color").pressed_index,
            mode_value,
            loop_enabled,
            direction_value,
            test_leaf,
            big_ready_a,
            big_ready_b,
            small_progress,
        )
        mode_text = self.mode_text(self.start_params[1])
        self.query_one("#state").update(self.build_state_text())
        log.write_line(
            "[Info] 正在发送启动参数，颜色方：%s，启动模式：%s，循环：%s，方向：%s，测试叶片：%d，大符待击打：%d,%d，小符进度：%d/4"
            % (
                "红方" if not self.start_params[0] else "蓝方",
                mode_text,
                "是" if self.start_params[2] else "否",
                "顺时针" if not self.start_params[3] else "逆时针",
                self.start_params[4],
                self.start_params[5],
                self.start_params[6],
                self.start_params[7],
            )
        )
        run_payload = bytes(
            [
                self.start_params[0],
                self.start_params[1],
                int(self.start_params[2]),
                self.start_params[3],
                self.start_params[4],
                self.start_params[5],
                self.start_params[6],
                self.start_params[7],
            ]
        )
        log.write_line("[Info] RUN payload: %s" % list(run_payload))
        await client.write_gatt_char(
            UUID_Char_RUN,
            run_payload,
        )
        self.set_running_state(True)

    def normalize_test_leaf(self, _mode_value: int, leaf_value) -> int:
        try:
            test_leaf = int(leaf_value)
        except (TypeError, ValueError):
            test_leaf = 1
        if test_leaf < 1 or test_leaf > 5:
            test_leaf = 1
        return test_leaf

    def normalize_small_progress(self, progress_value) -> int:
        try:
            progress = int(progress_value)
        except (TypeError, ValueError):
            progress = 0
        if progress < 0:
            progress = 0
        if progress > 4:
            progress = 4
        return progress

    def normalize_big_ready_leaf(self, leaf_value, fallback: int) -> int:
        try:
            leaf = int(leaf_value)
        except (TypeError, ValueError):
            leaf = fallback
        if leaf < 1 or leaf > 5:
            leaf = fallback
        return leaf

    def normalize_big_ready_pair(self, leaf_a_value, leaf_b_value):
        ready_a = self.normalize_big_ready_leaf(leaf_a_value, 1)
        ready_b = self.normalize_big_ready_leaf(leaf_b_value, 2)
        if ready_a == ready_b:
            ready_b = (ready_a % 5) + 1
            log.write_line(
                "[Info] 大符待击打叶片重复，自动修正为 A=%d, B=%d" % (ready_a, ready_b)
            )
        return ready_a, ready_b

    def resolve_run_parameters(self, mode_value: int):
        common_test_leaf = self.normalize_test_leaf(
            mode_value, self.query_one("#test_leaf", Select).value
        )
        small_ready_leaf = self.normalize_test_leaf(
            mode_value, self.query_one("#small_ready_leaf", Select).value
        )
        small_progress = self.normalize_small_progress(
            self.query_one("#small_progress", Select).value
        )
        big_progress = self.normalize_test_leaf(
            mode_value, self.query_one("#big_progress", Select).value
        )
        big_ready_a, big_ready_b = self.normalize_big_ready_pair(
            self.query_one("#big_ready_leaf_a", Select).value,
            self.query_one("#big_ready_leaf_b", Select).value,
        )

        if mode_value == self.RUN_MODE_SMALL_4_HIT_1_READY_TEST:
            return small_ready_leaf, big_ready_a, big_ready_b, small_progress
        if mode_value == self.RUN_MODE_BIG_PROGRESS_2_READY_TEST:
            return big_progress, big_ready_a, big_ready_b, 0
        return common_test_leaf, big_ready_a, big_ready_b, 0

    async def stop_run(self) -> None:
        await client.write_gatt_char(UUID_Char_Stop, bytes([0]))
        log.write_line("[Info] 正在发送停止指令...")
        self.set_running_state(False)
        self.query_one("#state").update("未运行")

    async def switch_mode(
        self,
        mode_value: int,
        loop_enabled: bool,
        direction_value: int,
        test_leaf: int,
        big_ready_a: int,
        big_ready_b: int,
        small_progress: int,
    ) -> None:
        log.write_line(
            "[ModeSwitch] switch_mode requested running=%s target_mode=%s(%d)"
            % ("yes" if self.running else "no", self.mode_text(mode_value), mode_value)
        )
        if self.running:
            self.notify("模式切换：先暂停当前模式...", title="提示", severity="information")
            log.write_line("[ModeSwitch] stop_run")
            await self.stop_run()
            await asyncio.sleep(0.2)
        log.write_line("[ModeSwitch] send_run")
        await self.send_run(
            mode_value,
            loop_enabled,
            direction_value,
            test_leaf,
            big_ready_a,
            big_ready_b,
            small_progress,
        )
        log.write_line("[ModeSwitch] mode switched")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        """An action to start the PowerRune."""
        if event.button.id == "run_toggle":
            if connected:
                if not self.running:
                    self.notify("正在发送启动参数...", title="提示", severity="information")
                    mode_value = self.selected_mode_value()
                    test_leaf, big_ready_a, big_ready_b, small_progress = self.resolve_run_parameters(mode_value)
                    await self.send_run(
                        mode_value,
                        self.query_one("#loop").value,
                        self.query_one("#direction").pressed_index,
                        test_leaf,
                        big_ready_a,
                        big_ready_b,
                        small_progress,
                    )
                else:
                    self.notify("正在发送停止指令...", title="提示", severity="information")
                    await self.stop_run()
            else:
                self.notify("设备未连接", title="错误", severity="error")
                log.write_line("[Error] 设备未连接")
        elif event.button.id == "func_all_target_ready":
            if connected:
                self.notify("正在发送功能1（全靶待击打）...", title="提示", severity="information")
                await self.switch_mode(self.RUN_MODE_ALL_TARGET_READY, False, 0, 1, 1, 2, 0)
            else:
                self.notify("设备未连接", title="错误", severity="error")
                log.write_line("[Error] 设备未连接")
        elif event.button.id == "func_all_success_static":
            if connected:
                self.notify("正在发送功能2（全面成功常亮）...", title="提示", severity="information")
                await self.switch_mode(self.RUN_MODE_ALL_SUCCESS_STATIC, False, 0, 1, 1, 2, 0)
            else:
                self.notify("设备未连接", title="错误", severity="error")
                log.write_line("[Error] 设备未连接")
        elif event.button.id == "clear":
            self.score_history.clear()
            self.refresh_score_table()
            log.write_line("[Info] 清空得分记录")

    async def on_radio_set_changed(self, event: RadioSet.Changed) -> None:
        radio_set = getattr(event, "radio_set", None)
        if radio_set is None:
            radio_set = getattr(event, "control", None)
        if radio_set is None or radio_set.id != "mode":
            return
        event_index = getattr(event, "pressed_index", None)
        if event_index is None:
            event_pressed = getattr(event, "pressed", None)
            if event_pressed is not None:
                try:
                    event_index = list(radio_set.query(RadioButton)).index(event_pressed)
                except Exception:
                    event_index = None
        event_mode = self.mode_value_from_index(event_index)
        new_mode = event_mode if event_mode is not None else self.selected_mode_value()
        log.write_line(
            "[ModeSwitch] mode changed event running=%s connected=%s event_index=%s target_mode=%s(%d)"
            % (
                "yes" if self.running else "no",
                "yes" if connected else "no",
                str(event_index),
                self.mode_text(new_mode),
                new_mode,
            )
        )
        if not connected or not self.running:
            log.write_line("[ModeSwitch] ignored: not running or not connected")
            return
        if new_mode == self.start_params[1]:
            log.write_line("[ModeSwitch] ignored: target mode equals current running mode")
            return
        test_leaf, big_ready_a, big_ready_b, small_progress = self.resolve_run_parameters(new_mode)
        self.notify("检测到模式切换，正在切换运行模式...", title="提示", severity="information")
        await self.switch_mode(
            new_mode,
            self.query_one("#loop").value,
            self.query_one("#direction").pressed_index,
            test_leaf,
            big_ready_a,
            big_ready_b,
            small_progress,
        )

    async def on_mount(self) -> None:
        self.first_notify_enabled = False
        self.running = False
        self.score_history = []
        self.start_params = (
            0,
            self.RUN_MODE_BIG,
            True,
            0,
            1,
            1,
            2,
            0,
        )
        table = self.query_one(DataTable)
        table.add_columns(*("颜色", "模式", "得分", "时间"))
        self.set_running_state(False)

    def refresh_score_table(self) -> None:
        table = self.query_one(DataTable)
        table.clear()
        for row in reversed(self.score_history):
            table.add_row(*row)

    def update_score(self, score: int, mode_name=None, armour_id=None) -> None:
        mode_text = mode_name or self.mode_text(self.start_params[1]).replace("模式", "")
        if armour_id is not None:
            mode_text = f"{mode_text}-靶{armour_id}"
        self.score_history.append(
            (
                "红方" if not self.start_params[0] else "蓝方",
                mode_text,
                score,
                time.strftime("%H:%M:%S", time.localtime()),
            )
        )
        self.refresh_score_table()


class PowerRune24_Settings(Static):
    """A widget to display the available settings of PowerRune."""

    def compose(self) -> ComposeResult:
        # 保存按钮
        with ScrollableContainer():
            yield Button("保存", id="save", variant="success")
            # yield LoadingIndicator()
            with Collapsible(title="网络和更新设置", collapsed=True):
                yield Label("更新服务器URL")
                yield Input(placeholder="请输入URL", id="url")

                yield Label("SSID")
                yield Input(placeholder="请输入SSID", id="ssid")

                yield Label("密码")
                yield Input(placeholder="请输入密码", id="psk")

                yield Label("自动OTA")
                yield Switch(value=True, id="auto_ota")
            with Collapsible(title="亮度设置", collapsed=True):

                yield Label("大符环数靶亮度")
                yield Select(((str(i), str(i)) for i in range(0, 256)), id="brightness")

                yield Label("大符臂亮度")
                yield Select(
                    ((str(i), str(i)) for i in range(0, 256)), id="brightness_arm"
                )

                yield Label("R标亮度")
                yield Select(
                    ((str(i), str(i)) for i in range(0, 256)), id="brightness_rlogo"
                )

                yield Label("点阵亮度")
                yield Select(
                    ((str(i), str(i)) for i in range(0, 256)), id="brightness_matrix"
                )
            with Collapsible(title="PID设置", collapsed=True):

                yield Label("kP值")
                yield Input(placeholder="请输入kP值", id="kp")
                yield Label("kI值")
                yield Input(placeholder="请输入kI值", id="ki")
                yield Label("kD值")
                yield Input(placeholder="请输入kD值", id="kd")
                # i_max, d_max, o_max

                yield Label("i_max值")
                yield Input(placeholder="请输入i_max值", id="i_max")
                yield Label("d_max值")
                yield Input(placeholder="请输入d_max值", id="d_max")
                yield Label("o_max值")
                yield Input(placeholder="请输入o_max值", id="o_max")

            with Collapsible(title="高级操作", collapsed=True):
                with Horizontal(id="advanced_buttons"):
                    yield Button("OTA", id="ota", variant="primary")
                    yield Button("重置装甲板ID", id="reset", variant="warning")


class PowerRune24_Operator(App):
    """A Textual app to manage stopwatches."""

    ENABLE_COMMAND_PALETTE = False
    CSS_PATH = "pr-24-operator.tcss"
    BINDINGS = [
        ("d", "toggle_dark", "颜色模式切换"),
        ("q", "quit", "退出"),
        ("c", "connect", "连接设备"),
    ]

    def compose(self) -> ComposeResult:
        """Create child widgets for the app."""
        yield Header(show_clock=True)
        with TabbedContent(initial="operations"):
            with TabPane("操作", id="operations"):  # First tab
                yield ScrollableContainer(PowerRune24_Operations())
            with TabPane("日志", id="logs"):
                yield Log(id="log")
                yield Button("清空日志", id="clear_log", variant="error")
            # with TabPane("系统设置", id="settings"):
            #     yield ScrollableContainer(PowerRune24_Settings())
        yield Footer()

    def on_button_pressed(self, button: Button.Pressed) -> None:
        """An action to clear the log."""
        if button.button.id == "clear_log":
            self.query_one(Log).clear()

    def action_toggle_dark(self) -> None:
        """An action to toggle dark mode."""
        self.dark = not self.dark

    async def action_connect(self) -> None:
        """An action to connect to the device."""
        if connected:
            self.notify("设备已连接", title="提示", severity="information")
            return
        if self.connect_task is not None and not self.connect_task.done():
            self.notify("设备正在连接", title="注意", severity="warning")
            return
        if self.connecting:
            self.connecting = False
        self.connect_task = asyncio.create_task(self.connect_device())

    async def action_quit(self) -> None:
        """An action to quit the app."""
        if connected and client is not None:
            try:
                await client.disconnect()
            except Exception:
                pass
        self.exit()

    def on_mount(self) -> None:
        """A lifecycle hook that runs when the app mounts."""
        global connected, client
        self.connecting = False
        self.devices = []
        self.device = None
        self.services = []
        self.characteristics = []
        self.descriptors = []
        self.service = None
        self.characteristic = None
        self.loop = asyncio.get_event_loop()
        client = None
        self.connect_task = None
        self.title = "PowerRune24 控制面板"
        connected = False
        self.sub_title = "未连接 - " + version

        global log
        log = self.query_one(Log)
        self.notify(
            "欢迎使用 PowerRune24 控制面板。",
            title="提示",
            severity="information",
        )
        log.write_line("[Info] 欢迎使用 PowerRune24 控制面板。")
        log.write_line("[Info] Script: %s version=%s" % (Path(__file__).resolve(), version))
        self.connect_task = asyncio.create_task(self.connect_device())

    @staticmethod
    def _normalize_name(name: str) -> str:
        if not isinstance(name, str):
            return ""
        return name.strip().lower()

    async def find_target_device(self):
        target_name = self._normalize_name(DEVICE_NAME)
        self.device = await BleakScanner.find_device_by_filter(
            lambda d, ad: self._normalize_name(getattr(d, "name", "")) == target_name
        )
        if self.device is not None:
            return self.device, "exact", []

        discovery = await BleakScanner.discover(timeout=5.0, return_adv=True)
        if isinstance(discovery, dict):
            scanned = list(discovery.values())
        else:
            scanned = [(item, None) for item in discovery]

        candidates = []
        scanned_preview = []
        for entry in scanned:
            if isinstance(entry, tuple) and len(entry) >= 2:
                device, adv_data = entry[0], entry[1]
            else:
                device, adv_data = entry, None
            name = getattr(device, "name", "") or ""
            local_name = getattr(adv_data, "local_name", "") if adv_data is not None else ""
            norm_name = self._normalize_name(name)
            norm_local_name = self._normalize_name(local_name)
            scanned_preview.append(
                "%s|n=%s|ln=%s"
                % (getattr(device, "address", "?"), name if name else "-", local_name if local_name else "-")
            )
            if (
                target_name in norm_name
                or norm_name.startswith(target_name)
                or target_name in norm_local_name
                or norm_local_name.startswith(target_name)
            ):
                candidates.append((device, name, local_name))

        if not candidates:
            return None, "scan_none", scanned_preview

        def candidate_rank(item):
            _, name, local_name = item
            norm_name = self._normalize_name(name)
            norm_local_name = self._normalize_name(local_name)
            if norm_name == target_name or norm_local_name == target_name:
                priority = 0
            elif norm_name.startswith(target_name) or norm_local_name.startswith(target_name):
                priority = 1
            else:
                priority = 2
            return (priority, len(name or ""), len(local_name or ""))

        candidates.sort(key=candidate_rank)
        selected = candidates[0]
        return selected[0], "fallback", scanned_preview

    async def connect_device(self):
        self.connecting = True
        global connected, client
        try:
            # BLE client
            # Name: PowerRune24
            self.notify("正在搜索设备...", title="提示", severity="information")
            log.write_line("[Info] 正在搜索设备...")
            try:
                self.device, match_stage, scan_preview = await self.find_target_device()
            except Exception as e:
                self.notify(str(e), title="错误", severity="error")
                log.write_line("[Error][scan_fail] " + str(e))
                connected = False
                self.sub_title = "未连接 - " + version
                return

            if self.device is None:
                self.notify("未找到设备", title="错误", severity="error")
                if scan_preview:
                    log.write_line("[Info][scan_preview] " + "; ".join(scan_preview[:8]))
                log.write_line("[Error][scan_none] 未找到设备: %s" % DEVICE_NAME)
                connected = False
                self.sub_title = "未连接 - " + version
                return

            log.write_line(
                "[Info][scan_%s] 选中设备 %s (name=%s)"
                % (match_stage, self.device.address, getattr(self.device, "name", ""))
            )
            if match_stage == "fallback" and scan_preview:
                log.write_line("[Info][scan_candidates] " + "; ".join(scan_preview[:8]))

            try:
                client = BleakClient(self.device, disconnected_callback=self.on_disconnect)
            except Exception as e:
                self.notify(str(e), title="错误", severity="error")
                log.write_line("[Error][client_init_fail] " + str(e))
                client = None
                connected = False
                self.sub_title = "未连接 - " + version
                return

            try:
                await client.connect()
            except Exception as e:
                self.notify(str(e), title="错误", severity="error")
                log.write_line("[Error][connect_fail] " + str(e))
                try:
                    await client.disconnect()
                except Exception as disconnect_error:
                    log.write_line("[Error][connect_fail_disconnect] " + str(disconnect_error))
                client = None
                connected = False
                self.sub_title = "未连接 - " + version
                return

            if client.is_connected:
                self.notify(
                    "成功连接到大符设备 %s" % self.device.address,
                    title="提示",
                    severity="information",
                )
                # Check the appearance in the generic attribute. If it's not 0x09F0, it means it's an unknown device, not PowerRune.
                # In this case, clear the client, select a device again, and raise an error.
                connected = True
                self.sub_title = "已连接 - " + version
                log.write_line("[Info] 成功连接到大符设备 %s" % self.device.address)
                ops = self.query_one(PowerRune24_Operations)
                ops.set_running_state(False)
                ops.query_one("#state", Label).update("未运行")
                await asyncio.sleep(1)
            else:
                self.notify(
                    "尝试连接大符设备 %s 失败" % self.device.address,
                    title="错误",
                    severity="error",
                )
                log.write_line("[Error][connect_fail] 尝试连接大符设备 %s 失败" % self.device.address)
                connected = False
                self.sub_title = "未连接 - " + version
        finally:
            self.connecting = False
            current_task = asyncio.current_task()
            if self.connect_task is current_task or (self.connect_task is not None and self.connect_task.done()):
                self.connect_task = None

    def on_disconnect(self, _client: BleakClient):
        # npyscreen notify_confirm
        global connected, client
        self.notify("设备已断开", title="提示", severity="information")
        log.write_line("[Info] 设备已断开")
        self.connecting = False
        self.connect_task = None
        connected = False
        self.sub_title = "未连接 - " + version
        client = None
        if self.device:
            self.device = None
        try:
            ops = self.query_one(PowerRune24_Operations)
            ops.set_running_state(False)
            ops.query_one("#state", Label).update("未运行")
        except Exception:
            pass


if __name__ == "__main__":
    app = PowerRune24_Operator()
    app.run()

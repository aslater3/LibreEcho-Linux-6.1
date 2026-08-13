#!/usr/bin/env python3
"""Source contract tests for the MT8163 Linux HCI bridge."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "drivers/misc/mediatek/mt8163-connectivity/conn_soc/common/linux/pub/stp_chrdev_bt.c"
HCI_HEADER = ROOT / "include/net/bluetooth/hci.h"
HCI_SOCK = ROOT / "net/bluetooth/hci_sock.c"
DEFCONFIG = ROOT / "arch/arm/configs/mt8163_arm32_defconfig"


class Mt8163BluetoothContractTests(unittest.TestCase):
    def test_defconfig_declares_complete_bluetooth_bridge(self) -> None:
        config = DEFCONFIG.read_text()
        required = (
            "CONFIG_BT=y",
            "CONFIG_BT_BREDR=y",
            "CONFIG_BT_LE=y",
            "CONFIG_RFKILL=y",
            "CONFIG_MTK_BTIF=y",
            "CONFIG_MTK_COMBO_BT=y",
            "CONFIG_MTK_MT8163_BLUEZ_HCI=y",
        )
        for symbol in required:
            with self.subTest(symbol=symbol):
                self.assertIn(f"{symbol}\n", config)

    def test_deferred_registration_allows_only_power_on_during_setup(self) -> None:
        driver = DRIVER.read_text()
        header = HCI_HEADER.read_text()
        sock = HCI_SOCK.read_text()
        self.assertIn("HCI_QUIRK_DEFERRED_SETUP", header)
        self.assertIn(
            "set_bit(HCI_QUIRK_DEFERRED_SETUP, &mtk_hci.hdev->quirks);",
            driver,
        )
        self.assertIn("opcode == MGMT_OP_SET_POWERED", sock)
        self.assertNotIn("hci_dev_clear_flag(mtk_hci.hdev, HCI_SETUP);", driver)

    def test_bridge_keeps_explicit_hci_lifecycle_contract(self) -> None:
        source = DRIVER.read_text()
        for marker in (
            "result = mtk_wcn_stp_register_if_rx(mtk_bt_hci_receive);",
            "result = mtk_wcn_stp_send_data(skb->data, skb->len,",
            "result = hci_recv_frame(mtk_hci.hdev, skb);",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, source)


if __name__ == "__main__":
    unittest.main()

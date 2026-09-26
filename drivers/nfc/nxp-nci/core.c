// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic driver for NXP NCI NFC chips
 *
 * Copyright (C) 2014  NXP Semiconductors  All rights reserved.
 *
 * Authors: Clément Perrochaud <clement.perrochaud@nxp.com>
 *
 * Derived from PN544 device driver:
 * Copyright (C) 2012  Intel Corporation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/nfc.h>

#include <net/nfc/nci_core.h>

#include "nxp-nci.h"

#define NXP_NCI_HDR_LEN	4

#define NXP_NCI_NFC_PROTOCOLS (NFC_PROTO_JEWEL_MASK | \
			       NFC_PROTO_MIFARE_MASK | \
			       NFC_PROTO_FELICA_MASK | \
			       NFC_PROTO_ISO14443_MASK | \
			       NFC_PROTO_ISO14443_B_MASK | \
			       NFC_PROTO_ISO15693_MASK | \
			       NFC_PROTO_NFC_DEP_MASK)

#define NXP_NCI_RF_PLL_UNLOCKED_NTF nci_opcode_pack(NCI_GID_RF_MGMT, 0x21)
#define NXP_NCI_RF_TXLDO_ERROR_NTF nci_opcode_pack(NCI_GID_RF_MGMT, 0x23)

static int nxp_nci_open(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r = 0;

	mutex_lock(&info->info_lock);

	if (info->mode != NXP_NCI_MODE_COLD) {
		r = -EBUSY;
		goto open_exit;
	}

	if (info->phy_ops->set_mode)
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_NCI);

	info->mode = NXP_NCI_MODE_NCI;

open_exit:
	mutex_unlock(&info->info_lock);
	return r;
}

static int nxp_nci_close(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r = 0;

	mutex_lock(&info->info_lock);

	if (info->phy_ops->set_mode)
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);

	info->mode = NXP_NCI_MODE_COLD;

	mutex_unlock(&info->info_lock);
	return r;
}

static int nxp_nci_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r;

	if (!info->phy_ops->write) {
		kfree_skb(skb);
		return -EOPNOTSUPP;
	}

	if (info->mode != NXP_NCI_MODE_NCI) {
		kfree_skb(skb);
		return -EINVAL;
	}

	r = info->phy_ops->write(info->phy_id, skb);
	if (r < 0) {
		kfree_skb(skb);
		return r;
	}

	consume_skb(skb);
	return 0;
}

static int nxp_nci_rf_pll_unlocked_ntf(struct nci_dev *ndev,
				       struct sk_buff *skb)
{
	nfc_err(&ndev->nfc_dev->dev,
		"PLL didn't lock. Missing or unstable clock?\n");

	return 0;
}

static int nxp_nci_rf_txldo_error_ntf(struct nci_dev *ndev,
				      struct sk_buff *skb)
{
	nfc_err(&ndev->nfc_dev->dev,
		"RF transmitter couldn't start. Bad power and/or configuration?\n");

	return 0;
}

static const struct nci_driver_ops nxp_nci_core_ops[] = {
	{
		.opcode = NXP_NCI_RF_PLL_UNLOCKED_NTF,
		.ntf = nxp_nci_rf_pll_unlocked_ntf,
	},
	{
		.opcode = NXP_NCI_RF_TXLDO_ERROR_NTF,
		.ntf = nxp_nci_rf_txldo_error_ntf,
	},
};

/*
 * NCI leaves RF protocol values 0x80-0xff to the controller vendor, so the core
 * hands them back to us. The ST54J reports MIFARE Classic as 0x90: a card that
 * activates with it answers ATQA 0x0004 / SAK 0x08, which is MIFARE Classic 1K.
 * Without this mapping the core has no protocol to offer and drops the target
 * with "the target found does not have the desired protocol", even though the
 * controller activated it and granted a data connection.
 */
#define ST54J_RF_PROTOCOL_MIFARE_CLASSIC 0x90

static __u32 nxp_nci_st54j_get_rfprotocol(struct nci_dev *ndev,
					  __u8 rf_protocol)
{
	if (rf_protocol == ST54J_RF_PROTOCOL_MIFARE_CLASSIC)
		return NFC_PROTO_MIFARE_MASK;

	return 0;
}

/*
 * After CORE_INIT the ST54J must be switched to NFC mode ON with a proprietary
 * command, as ST's own stack does. Without it the controller stays usable for a
 * few seconds and then drops into a low-power state from which it no longer
 * acknowledges its I2C address, so every later command fails with -ENXIO until
 * VEN is cycled.
 *
 * The controller answers the command with a response and then resets itself,
 * announcing it with CORE_RESET_NTF. The response therefore only completes the
 * request on error; on success the core's CORE_RESET_NTF handler completes it,
 * and the controller is then initialised again.
 */
#define ST54J_PROP_NFC_MODE_OID		0x02
#define ST54J_PROP_NFC_MODE_SET		0x02
#define ST54J_NFC_MODE_ON		0x01

static int nxp_nci_st54j_prop_mode_rsp(struct nci_dev *ndev,
				       struct sk_buff *skb)
{
	if (skb->data[0] != NCI_STATUS_OK)
		nci_req_complete(ndev, skb->data[0]);

	return 0;
}

static const struct nci_driver_ops nxp_nci_st54j_prop_ops[] = {
	{
		.opcode = nci_opcode_pack(NCI_GID_PROPRIETARY,
					  ST54J_PROP_NFC_MODE_OID),
		.rsp = nxp_nci_st54j_prop_mode_rsp,
	},
};

static int nxp_nci_st54j_post_setup(struct nci_dev *ndev)
{
	static const __u8 mode_on[] = { ST54J_PROP_NFC_MODE_SET,
					ST54J_NFC_MODE_ON };
	struct nci_core_init_v2_cmd init = {
		.feature1 = NCI_FEATURE_DISABLE,
		.feature2 = NCI_FEATURE_DISABLE,
	};
	int r;

	r = nci_prop_cmd(ndev, ST54J_PROP_NFC_MODE_OID, sizeof(mode_on),
			 mode_on);
	if (r)
		return r;

	return nci_core_cmd(ndev, NCI_OP_CORE_INIT_CMD, sizeof(init),
			    (__u8 *)&init);
}

/* NXP controllers: reachable firmware-download mode, no vendor RF protocols. */
static const struct nci_ops nxp_nci_ops = {
	.open = nxp_nci_open,
	.close = nxp_nci_close,
	.send = nxp_nci_send,
	.fw_download = nxp_nci_fw_download,
	.core_ops = nxp_nci_core_ops,
	.n_core_ops = ARRAY_SIZE(nxp_nci_core_ops),
};

/*
 * The ST54J has no NXP firmware-download mode, so .fw_download is left out and
 * the NFC core never offers it (a download would otherwise push NXP vendor
 * frames to the ST part). It does report a proprietary RF protocol to map, and
 * needs NFC mode switched on after CORE_INIT.
 */
static const struct nci_ops nxp_nci_st54j_ops = {
	.open = nxp_nci_open,
	.close = nxp_nci_close,
	.send = nxp_nci_send,
	.post_setup = nxp_nci_st54j_post_setup,
	.get_rfprotocol = nxp_nci_st54j_get_rfprotocol,
	.prop_ops = nxp_nci_st54j_prop_ops,
	.n_prop_ops = ARRAY_SIZE(nxp_nci_st54j_prop_ops),
	.core_ops = nxp_nci_core_ops,
	.n_core_ops = ARRAY_SIZE(nxp_nci_core_ops),
};

int nxp_nci_probe(void *phy_id, struct device *pdev,
		  const struct nxp_nci_phy_ops *phy_ops,
		  unsigned int max_payload, enum nxp_nci_variant variant,
		  struct nci_dev **ndev)
{
	const struct nci_ops *ops = &nxp_nci_ops;
	struct nxp_nci_info *info;
	int r;

	if (variant == NXP_NCI_ST54J)
		ops = &nxp_nci_st54j_ops;

	info = devm_kzalloc(pdev, sizeof(struct nxp_nci_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->phy_id = phy_id;
	info->pdev = pdev;
	info->phy_ops = phy_ops;
	info->max_payload = max_payload;
	INIT_WORK(&info->fw_info.work, nxp_nci_fw_work);
	init_completion(&info->fw_info.cmd_completion);
	mutex_init(&info->info_lock);

	if (info->phy_ops->set_mode) {
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);
		if (r < 0)
			return r;
	}

	info->mode = NXP_NCI_MODE_COLD;

	info->ndev = nci_allocate_device(ops, NXP_NCI_NFC_PROTOCOLS,
					 NXP_NCI_HDR_LEN, 0);
	if (!info->ndev)
		return -ENOMEM;

	nci_set_parent_dev(info->ndev, pdev);
	nci_set_drvdata(info->ndev, info);
	r = nci_register_device(info->ndev);
	if (r < 0) {
		nci_free_device(info->ndev);
		return r;
	}

	*ndev = info->ndev;
	return r;
}
EXPORT_SYMBOL(nxp_nci_probe);

void nxp_nci_remove(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);

	if (info->mode == NXP_NCI_MODE_FW)
		nxp_nci_fw_work_complete(info, -ESHUTDOWN);
	cancel_work_sync(&info->fw_info.work);

	/*
	 * Not under info_lock: if the device is up, unregistering closes it,
	 * which resets the controller (the IRQ thread takes info_lock to
	 * deliver the response) and then calls nxp_nci_close(), which takes
	 * info_lock itself.
	 */
	nci_unregister_device(ndev);

	mutex_lock(&info->info_lock);

	if (info->phy_ops->set_mode)
		info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);

	mutex_unlock(&info->info_lock);

	nci_free_device(ndev);
}
EXPORT_SYMBOL(nxp_nci_remove);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NXP NCI NFC driver");
MODULE_AUTHOR("Clément Perrochaud <clement.perrochaud@nxp.com>");

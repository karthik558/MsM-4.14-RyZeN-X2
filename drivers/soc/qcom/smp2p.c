/*
 * Copyright (c) 2015, Sony Mobile Communications AB.
 * Copyright (c) 2012-2013, 2018-2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mailbox_client.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/spinlock.h>
#include <linux/pm_wakeup.h>

#include <linux/ipc_logging.h>

/*
 * The Shared Memory Point to Point (SMP2P) protocol facilitates communication
 * of a single 32-bit value between two processors.  Each value has a single
 * writer (the local side) and a single reader (the remote side). Values are
 * uniquely identified in the system by the directed edge (local processor ID
 * to remote processor ID) and a string identifier.
 *
 * Each processor is responsible for creating the outgoing SMEM items and each
 * item is writable by the local processor and readable by the remote
 * processor.  By using two separate SMEM items that are single-reader and
 * single-writer, SMP2P does not require any remote locking mechanisms.
 *
 * The driver uses the Linux GPIO and interrupt framework to expose a virtual
 * GPIO for each outbound entry and a virtual interrupt controller for each
 * inbound entry.
 */

#define SMP2P_MAX_ENTRY 16
#define SMP2P_MAX_ENTRY_NAME 16

#define SMP2P_FEATURE_SSR_ACK 0x1
#define SMP2P_FLAGS_RESTART_DONE_BIT 0
#define SMP2P_FLAGS_RESTART_ACK_BIT 1

#define SMP2P_MAGIC 0x504d5324
#define SMP2P_VERSION	1
#define SMP2P_FEATURES	SMP2P_FEATURE_SSR_ACK

/**
 * struct smp2p_smem_item - in memory communication structure
 * @magic:		magic number
 * @version:		version - must be 1
 * @features:		features flag - currently unused
 * @local_pid:		processor id of sending end
 * @remote_pid:		processor id of receiving end
 * @total_entries:	number of entries - always SMP2P_MAX_ENTRY
 * @valid_entries:	number of allocated entries
 * @flags:
 * @entries:		individual communication entries
 *     @name:		name of the entry
 *     @value:		content of the entry
 */
struct smp2p_smem_item {
	u32 magic;
	u8 version;
	unsigned features:24;
	u16 local_pid;
	u16 remote_pid;
	u16 total_entries;
	u16 valid_entries;
	u32 flags;

	struct {
		u8 name[SMP2P_MAX_ENTRY_NAME];
		u32 value;
	} entries[SMP2P_MAX_ENTRY];
} __packed;

/**
 * struct smp2p_entry - driver context matching one entry
 * @node:	list entry to keep track of allocated entries
 * @smp2p:	reference to the device driver context
 * @name:	name of the entry, to match against smp2p_smem_item
 * @value:	pointer to smp2p_smem_item entry value
 * @last_value:	last handled value
 * @domain:	irq_domain for inbound entries
 * @irq_enabled:bitmap to track enabled irq bits
 * @irq_rising:	bitmap to mark irq bits for rising detection
 * @irq_falling:bitmap to mark irq bits for falling detection
 * @state:	smem state handle
 * @lock:	spinlock to protect read-modify-write of the value
 */
struct smp2p_entry {
	struct list_head node;
	struct qcom_smp2p *smp2p;

	const char *name;
	u32 *value;
	u32 last_value;

	struct irq_domain *domain;
	DECLARE_BITMAP(irq_enabled, 32);
	DECLARE_BITMAP(irq_pending, 32);
	DECLARE_BITMAP(irq_rising, 32);
	DECLARE_BITMAP(irq_falling, 32);

	struct qcom_smem_state *state;

	spinlock_t lock;
};

#define SMP2P_INBOUND	0
#define SMP2P_OUTBOUND	1

/**
 * struct qcom_smp2p - device driver context
 * @dev:	device driver handle
 * @in:		pointer to the inbound smem item
 * @smem_items:	ids of the two smem items
 * @valid_entries: already scanned inbound entries
 * @local_pid:	processor id of the inbound edge
 * @remote_pid:	processor id of the outbound edge
 * @ipc_regmap:	regmap for the outbound ipc
 * @ipc_offset:	offset within the regmap
 * @ipc_bit:	bit in regmap@offset to kick to signal remote processor
 * @mbox_client: mailbox client handle
 * @mbox_chan:	apcs ipc mailbox channel handle
 * @inbound:	list of inbound entries
 * @outbound:	list of outbound entries
 */
struct qcom_smp2p {
	struct device *dev;

	struct smp2p_smem_item *in;
	struct smp2p_smem_item *out;

	unsigned smem_items[SMP2P_OUTBOUND + 1];

	unsigned valid_entries;

	bool ssr_ack_enabled;
	bool ssr_ack;
	bool open;

	unsigned local_pid;
	unsigned remote_pid;

	int irq;
	struct regmap *ipc_regmap;
	int ipc_offset;
	int ipc_bit;
	struct wakeup_source ws;

	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;

	struct list_head inbound;
	struct list_head outbound;
};

static void *ilc;
#define SMP2P_LOG_PAGE_CNT 2
#define SMP2P_INFO(x, ...)	\
do {	\
	if (ilc) \
		ipc_log_string(ilc, "[%s]: "x, __func__, ##__VA_ARGS__); \
} while (0)

static void qcom_smp2p_kick(struct qcom_smp2p *smp2p)
{
	/* Make sure any updated data is written before the kick */
	wmb();

	if (smp2p->mbox_chan) {
		mbox_send_message(smp2p->mbox_chan, NULL);
		mbox_client_txdone(smp2p->mbox_chan, 0);
	} else {
		regmap_write(smp2p->ipc_regmap, smp2p->ipc_offset, BIT(smp2p->ipc_bit));
	}
}

static bool qcom_smp2p_check_ssr(struct qcom_smp2p *smp2p)
{
	struct smp2p_smem_item *in = smp2p->in;
	bool restart;

	if (!smp2p->ssr_ack_enabled)
		return false;

	restart = in->flags & BIT(SMP2P_FLAGS_RESTART_DONE_BIT);
	if (restart == smp2p->ssr_ack)
		return false;

	SMP2P_INFO("%d: SSR DETECTED\n", smp2p->remote_pid);
	return true;
}

static void qcom_smp2p_do_ssr_ack(struct qcom_smp2p *smp2p)
{
	struct smp2p_smem_item *out = smp2p->out;
	u32 ack = !smp2p->ssr_ack;
	u32 val;

	smp2p->ssr_ack = ack;
	ack = ack << SMP2P_FLAGS_RESTART_ACK_BIT;

	val = out->flags & ~BIT(SMP2P_FLAGS_RESTART_ACK_BIT);
	val |= ack;
	out->flags = val;

	qcom_smp2p_kick(smp2p);
}

static void qcom_smp2p_negotiate(struct qcom_smp2p *smp2p)
{
	struct smp2p_smem_item *out = smp2p->out;
	struct smp2p_smem_item *in = smp2p->in;
	u32 features;

	if (in->version == out->version) {
		features = in->features & out->features;
		out->features = features;

		if (features & SMP2P_FEATURE_SSR_ACK)
			smp2p->ssr_ack_enabled = true;

		smp2p->open = true;
		SMP2P_INFO("%d: state=open ssr_ack=%d\n", smp2p->remote_pid,
			   smp2p->ssr_ack_enabled);
	}
}

static void qcom_smp2p_notify_in(struct qcom_smp2p *smp2p)
{
	struct smp2p_smem_item *in = smp2p->in;
	struct smp2p_entry *entry;
	unsigned long status;
	int irq_pin;
	char buf[SMP2P_MAX_ENTRY_NAME];
	u32 val;
	int i;

	/* Match newly created entries */
	for (i = smp2p->valid_entries; i < in->valid_entries; i++) {
		list_for_each_entry(entry, &smp2p->inbound, node) {
			memcpy(buf, in->entries[i].name, sizeof(buf));
			if (!strcmp(buf, entry->name)) {
				entry->value = &in->entries[i].value;
				break;
			}
		}
	}
	smp2p->valid_entries = i;

	/* Fire interrupts based on any value changes */
	list_for_each_entry(entry, &smp2p->inbound, node) {
		/* Ignore entries not yet allocated by the remote side */
		if (!entry->value)
			continue;

		val = readl(entry->value);

		status = val ^ entry->last_value;
		entry->last_value = val;
		status |= *entry->irq_pending;

		/* No changes of this entry? */
		if (!status)
			continue;

		SMP2P_INFO("%d: %s: status:%0lx val:%0x\n",
			   smp2p->remote_pid, entry->name, status, val);

		for_each_set_bit(i, &status, 32) {
			if ((val & BIT(i) && test_bit(i, entry->irq_rising)) ||
			    (!(val & BIT(i)) && test_bit(i, entry->irq_falling))) {
				irq_pin = irq_find_mapping(entry->domain, i);
				handle_nested_irq(irq_pin);

				if (test_bit(i, entry->irq_enabled))
					clear_bit(i, entry->irq_pending);
				else
					set_bit(i, entry->irq_pending);
			}
		}
	}
}

static irqreturn_t qcom_smp2p_isr(int irq, void *data)
{
	struct qcom_smp2p *smp2p = data;

	__pm_stay_awake(&smp2p->ws);
	return IRQ_WAKE_THREAD;
}

/**
 * qcom_smp2p_intr() - interrupt handler for incoming notifications
 * @irq:	unused
 * @data:	smp2p driver context
 *
 * Handle notifications from the remote side to handle newly allocated entries
 * or any changes to the state bits of existing entries.
 */
static irqreturn_t qcom_smp2p_intr(int irq, void *data)
{
	struct smp2p_smem_item *in;
	struct qcom_smp2p *smp2p = data;
	unsigned int smem_id = smp2p->smem_items[SMP2P_INBOUND];
	unsigned int pid = smp2p->remote_pid;
	size_t size;

	in = smp2p->in;

	/* Acquire smem item, if not already found */
	if (!in) {
		in = qcom_smem_get(pid, smem_id, &size);
		if (IS_ERR(in)) {
			dev_err(smp2p->dev,
				"Unable to acquire remote smp2p item\n");
			goto out;
		}

		smp2p->in = in;
	}

	if (!smp2p->open)
		qcom_smp2p_negotiate(smp2p);

	if (smp2p->open) {
		bool do_restart;

		do_restart = qcom_smp2p_check_ssr(smp2p);
		qcom_smp2p_notify_in(smp2p);

		if (do_restart)
			qcom_smp2p_do_ssr_ack(smp2p);
	}

out:
	__pm_relax(&smp2p->ws);
	return IRQ_HANDLED;
}

static void smp2p_mask_irq(struct irq_data *irqd)
{
	struct smp2p_entry *entry = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t irq = irqd_to_hwirq(irqd);

	clear_bit(irq, entry->irq_enabled);
}

static void smp2p_unmask_irq(struct irq_data *irqd)
{
	struct smp2p_entry *entry = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t irq = irqd_to_hwirq(irqd);

	set_bit(irq, entry->irq_enabled);
}

static int smp2p_set_irq_type(struct irq_data *irqd, unsigned int type)
{
	struct smp2p_entry *entry = irq_data_get_irq_chip_data(irqd);
	irq_hw_number_t irq = irqd_to_hwirq(irqd);

	if (!(type & IRQ_TYPE_EDGE_BOTH))
		return -EINVAL;

	if (type & IRQ_TYPE_EDGE_RISING)
		set_bit(irq, entry->irq_rising);
	else
		clear_bit(irq, entry->irq_rising);

	if (type & IRQ_TYPE_EDGE_FALLING)
		set_bit(irq, entry->irq_falling);
	else
		clear_bit(irq, entry->irq_falling);

	return 0;
}

static struct irq_chip smp2p_irq_chip = {
	.name           = "smp2p",
	.irq_mask       = smp2p_mask_irq,
	.irq_unmask     = smp2p_unmask_irq,
	.irq_set_type	= smp2p_set_irq_type,
};

static int smp2p_irq_map(struct irq_domain *d,
			 unsigned int irq,
			 irq_hw_number_t hw)
{
	struct smp2p_entry *entry = d->host_data;

	irq_set_chip_and_handler(irq, &smp2p_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, entry);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);
	irq_set_parent(irq, entry->smp2p->irq);
	irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY);

	return 0;
}

static const struct irq_domain_ops smp2p_irq_ops = {
	.map = smp2p_irq_map,
	.xlate = irq_domain_xlate_twocell,
};

static int qcom_smp2p_inbound_entry(struct qcom_smp2p *smp2p,
				    struct smp2p_entry *entry,
				    struct device_node *node)
{
	entry->domain = irq_domain_add_linear(node, 32, &smp2p_irq_ops, entry);
	if (!entry->domain) {
		dev_err(smp2p->dev, "failed to add irq_domain\n");
		return -ENOMEM;
	}

	return 0;
}

static int smp2p_update_bits(void *data, u32 mask, u32 value)
{
	struct smp2p_entry *entry = data;
	u32 orig;
	u32 val;

	spin_lock(&entry->lock);
	val = orig = readl(entry->value);
	val &= ~mask;
	val |= value;
	writel(val, entry->value);
	spin_unlock(&entry->lock);
	SMP2P_INFO("%d: %s: orig:0x%0x new:0x%0x\n",
		   entry->smp2p->remote_pid, entry->name, orig, val);

	if (val != orig)
		qcom_smp2p_kick(entry->smp2p);

	return 0;
}

static const struct qcom_smem_state_ops smp2p_state_ops = {
	.update_bits = smp2p_update_bits,
};

static int qcom_smp2p_outbound_entry(struct qcom_smp2p *smp2p,
				     struct smp2p_entry *entry,
				     struct device_node *node)
{
	struct smp2p_smem_item *out = smp2p->out;
	char buf[SMP2P_MAX_ENTRY_NAME] = {};

	/* Allocate an entry from the smem item */
	strlcpy(buf, entry->name, SMP2P_MAX_ENTRY_NAME);
	memcpy(out->entries[out->valid_entries].name, buf, SMP2P_MAX_ENTRY_NAME);

	/* Make the logical entry reference the physical value */
	entry->value = &out->entries[out->valid_entries].value;

	out->valid_entries++;

	entry->state = qcom_smem_state_register(node, &smp2p_state_ops, entry);
	if (IS_ERR(entry->state)) {
		dev_err(smp2p->dev, "failed to register qcom_smem_state\n");
		return PTR_ERR(entry->state);
	}

	return 0;
}

static int qcom_smp2p_alloc_outbound_item(struct qcom_smp2p *smp2p)
{
	struct smp2p_smem_item *out;
	unsigned smem_id = smp2p->smem_items[SMP2P_OUTBOUND];
	unsigned pid = smp2p->remote_pid;
	int ret;

	ret = qcom_smem_alloc(pid, smem_id, sizeof(*out));
	if (ret < 0 && ret != -EEXIST) {
		if (ret != -EPROBE_DEFER)
			dev_err(smp2p->dev,
				"unable to allocate local smp2p item\n");
		return ret;
	}

	out = qcom_smem_get(pid, smem_id, NULL);
	if (IS_ERR(out)) {
		dev_err(smp2p->dev, "Unable to acquire local smp2p item\n");
		return PTR_ERR(out);
	}

	memset(out, 0, sizeof(*out));
	out->magic = SMP2P_MAGIC;
	out->local_pid = smp2p->local_pid;
	out->remote_pid = smp2p->remote_pid;
	out->total_entries = SMP2P_MAX_ENTRY;
	out->valid_entries = 0;
	out->features = SMP2P_FEATURES;

	/*
	 * Make sure the rest of the header is written before we validate the
	 * item by writing a valid version number.
	 */
	wmb();
	out->version = SMP2P_VERSION;

	qcom_smp2p_kick(smp2p);

	smp2p->out = out;

	return 0;
}

static int smp2p_parse_ipc(struct qcom_smp2p *smp2p)
{
	struct device_node *syscon;
	struct device *dev = smp2p->dev;
	const char *key;
	int ret;

	syscon = of_parse_phandle(dev->of_node, "qcom,ipc", 0);
	if (!syscon) {
		dev_err(dev, "no qcom,ipc node\n");
		return -ENODEV;
	}

	smp2p->ipc_regmap = syscon_node_to_regmap(syscon);
	if (IS_ERR(smp2p->ipc_regmap))
		return PTR_ERR(smp2p->ipc_regmap);

	key = "qcom,ipc";
	ret = of_property_read_u32_index(dev->of_node, key, 1, &smp2p->ipc_offset);
	if (ret < 0) {
		dev_err(dev, "no offset in %s\n", key);
		return -EINVAL;
	}

	ret = of_property_read_u32_index(dev->of_node, key, 2, &smp2p->ipc_bit);
	if (ret < 0) {
		dev_err(dev, "no bit in %s\n", key);
		return -EINVAL;
	}

	return 0;
}

static int qcom_smp2p_alloc_item(struct platform_device *pdev,
					struct qcom_smp2p *smp2p)
{
	int ret = 0;
	struct smp2p_entry *entry;
	struct device_node *node;

	ret = qcom_smp2p_alloc_outbound_item(smp2p);
	if (ret < 0)
		return ret;

	for_each_available_child_of_node(pdev->dev.of_node, node) {
		entry = devm_kzalloc(&pdev->dev, sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return -ENOMEM;

		entry->smp2p = smp2p;
		spin_lock_init(&entry->lock);

		ret = of_property_read_string(node, "qcom,entry-name",
							&entry->name);
		if (ret < 0)
			return ret;

		if (of_property_read_bool(node, "interrupt-controller")) {
			ret = qcom_smp2p_inbound_entry(smp2p, entry, node);
			if (ret < 0)
				return ret;

			list_add(&entry->node, &smp2p->inbound);
		} else {
			ret = qcom_smp2p_outbound_entry(smp2p, entry, node);
			if (ret < 0)
				return ret;

			list_add(&entry->node, &smp2p->outbound);
		}
	}
	wakeup_source_init(&smp2p->ws, "smp2p");

	/* Kick the outgoing edge after allocating entries */
	qcom_smp2p_kick(smp2p);

	return ret;
}

static int qcom_smp2p_probe(struct platform_device *pdev)
{
	struct smp2p_entry *entry;
	struct qcom_smp2p *smp2p;
	const char *key;
	int ret;

	if (!ilc)
		ilc = ipc_log_context_create(SMP2P_LOG_PAGE_CNT, "smp2p", 0);

	smp2p = devm_kzalloc(&pdev->dev, sizeof(*smp2p), GFP_KERNEL);
	if (!smp2p)
		return -ENOMEM;

	smp2p->dev = &pdev->dev;
	INIT_LIST_HEAD(&smp2p->inbound);
	INIT_LIST_HEAD(&smp2p->outbound);

	platform_set_drvdata(pdev, smp2p);

	key = "qcom,smem";
	ret = of_property_read_u32_array(pdev->dev.of_node, key,
					 smp2p->smem_items, 2);
	if (ret)
		return ret;

	key = "qcom,local-pid";
	ret = of_property_read_u32(pdev->dev.of_node, key, &smp2p->local_pid);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to read %s\n", key);
		return -EINVAL;
	}

	key = "qcom,remote-pid";
	ret = of_property_read_u32(pdev->dev.of_node, key, &smp2p->remote_pid);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to read %s\n", key);
		return -EINVAL;
	}

	smp2p->irq = platform_get_irq(pdev, 0);
	if (smp2p->irq < 0) {
		dev_err(&pdev->dev, "unable to acquire smp2p interrupt\n");
		return smp2p->irq;
	}

	smp2p->mbox_client.dev = &pdev->dev;
	smp2p->mbox_client.knows_txdone = true;
	smp2p->mbox_chan = mbox_request_channel(&smp2p->mbox_client, 0);
	if (IS_ERR(smp2p->mbox_chan)) {
		if (PTR_ERR(smp2p->mbox_chan) != -ENODEV)
			return PTR_ERR(smp2p->mbox_chan);

		smp2p->mbox_chan = NULL;

		ret = smp2p_parse_ipc(smp2p);
		if (ret)
			return ret;
	}

	ret = qcom_smp2p_alloc_item(pdev, smp2p);
	if (ret < 0 && ret != -EEXIST) {
		if (ret != -EPROBE_DEFER) {
			dev_err(&pdev->dev, "failed to alloc outbound items\n");
			goto release_mbox;
		}
	} else if (ret) {
		dev_err(&pdev->dev, "failed to get smp2p entries\n");
		goto unwind_interfaces;
	}

	ret = devm_request_threaded_irq(&pdev->dev, smp2p->irq,
					qcom_smp2p_isr, qcom_smp2p_intr,
					IRQF_ONESHOT,
					"smp2p", (void *)smp2p);
	if (ret) {
		dev_err(&pdev->dev, "failed to request interrupt\n");
		goto unwind_interfaces;
	}

	return 0;

unwind_interfaces:
	list_for_each_entry(entry, &smp2p->inbound, node)
		irq_domain_remove(entry->domain);

	list_for_each_entry(entry, &smp2p->outbound, node)
		qcom_smem_state_unregister(entry->state);

	smp2p->out->valid_entries = 0;

release_mbox:
	mbox_free_channel(smp2p->mbox_chan);

	return ret;
}

static int qcom_smp2p_remove(struct platform_device *pdev)
{
	struct qcom_smp2p *smp2p = platform_get_drvdata(pdev);
	struct smp2p_entry *entry;

	list_for_each_entry(entry, &smp2p->inbound, node)
		irq_domain_remove(entry->domain);

	list_for_each_entry(entry, &smp2p->outbound, node)
		qcom_smem_state_unregister(entry->state);

	mbox_free_channel(smp2p->mbox_chan);

	smp2p->out->valid_entries = 0;

	return 0;
}

static int qcom_smp2p_resume(struct device *dev)
{
	int ret = 0;
	struct qcom_smp2p *smp2p = dev_get_drvdata(dev);
	struct smp2p_entry *entry;
	struct device_node *node;
	struct platform_device *pdev = container_of(dev, struct
						platform_device, dev);

	ret = qcom_smp2p_alloc_outbound_item(smp2p);
	if (ret < 0)
		goto print_err;

	for_each_available_child_of_node(pdev->dev.of_node, node) {
		entry = devm_kzalloc(&pdev->dev, sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			ret = -ENOMEM;
			goto print_err;
		}

		entry->smp2p = smp2p;
		spin_lock_init(&entry->lock);
		ret = of_property_read_string(node, "qcom,entry-name",
								&entry->name);
		if (ret < 0)
			goto print_err;

		if (!of_property_read_bool(node, "interrupt-controller")) {
			ret = qcom_smp2p_outbound_entry(smp2p, entry, node);
			if (ret < 0)
				goto print_err;

			list_add(&entry->node, &smp2p->outbound);
		}
	}
	wakeup_source_init(&smp2p->ws, "smp2p");

	/* Kick the outgoing edge after allocating entries */
	qcom_smp2p_kick(smp2p);

print_err:
	if (ret < 0 && ret != -EEXIST)
		dev_err(dev, "failed to alloc items ret = %d\n", ret);

	return ret;
}

static int qcom_smp2p_suspend(struct device *dev)
{
	struct qcom_smp2p *smp2p = dev_get_drvdata(dev);
	struct smp2p_entry *entry;
	struct smp2p_entry *next_entry;

	/* Walk through the out bound list and release state and entry */
	list_for_each_entry_safe(entry, next_entry, &smp2p->outbound, node) {
		qcom_smem_state_unregister(entry->state);
		list_del(&entry->node);
		devm_kfree(smp2p->dev, entry);
	}
	INIT_LIST_HEAD(&smp2p->outbound);

	/* Walk through the in bound list and reset last value */
	list_for_each_entry_safe(entry, next_entry, &smp2p->inbound, node) {
		entry->last_value = 0;
	}
	/* make null to point it to valid smem item during first interrupt */
	smp2p->in = NULL;
	smp2p->valid_entries = 0;
	/* remove wakeup source */
	wakeup_source_trash(&smp2p->ws);
	return 0;
}

static const struct dev_pm_ops qcom_smp2p_pm_ops = {
	.freeze = qcom_smp2p_suspend,
	.restore = qcom_smp2p_resume,
};

static const struct of_device_id qcom_smp2p_of_match[] = {
	{ .compatible = "qcom,smp2p" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_smp2p_of_match);

static struct platform_driver qcom_smp2p_driver = {
	.probe = qcom_smp2p_probe,
	.remove = qcom_smp2p_remove,
	.driver  = {
		.name  = "qcom_smp2p",
		.of_match_table = qcom_smp2p_of_match,
		.pm = &qcom_smp2p_pm_ops,
	},
};
module_platform_driver(qcom_smp2p_driver);

MODULE_DESCRIPTION("Qualcomm Shared Memory Point to Point driver");
MODULE_LICENSE("GPL v2");

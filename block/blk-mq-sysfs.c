#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/smp.h>

#include <linux/blk-mq.h>
#include "blk-mq.h"
#include "blk-mq-tag.h"
#include "blk.h"

static void blk_mq_sysfs_release(struct kobject *kobj)
{
}

struct blk_mq_ctx_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct blk_mq_ctx *, char *);
	ssize_t (*store)(struct blk_mq_ctx *, const char *, size_t);
};

struct blk_mq_hw_ctx_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct blk_mq_hw_ctx *, char *);
	ssize_t (*store)(struct blk_mq_hw_ctx *, const char *, size_t);
};

static ssize_t blk_mq_sysfs_show(struct kobject *kobj, struct attribute *attr,
				 char *page)
{
	struct blk_mq_ctx_sysfs_entry *entry;
	struct blk_mq_ctx *ctx;
	struct request_queue *q;
	ssize_t res;

	entry = container_of(attr, struct blk_mq_ctx_sysfs_entry, attr);
	ctx = container_of(kobj, struct blk_mq_ctx, kobj);
	q = ctx->queue;

	if (!entry->show)
		return -EIO;

	res = -ENOENT;
	mutex_lock(&q->sysfs_lock);
	if (!blk_queue_dying(q))
		res = entry->show(ctx, page);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static ssize_t blk_mq_sysfs_store(struct kobject *kobj, struct attribute *attr,
				  const char *page, size_t length)
{
	struct blk_mq_ctx_sysfs_entry *entry;
	struct blk_mq_ctx *ctx;
	struct request_queue *q;
	ssize_t res;

	entry = container_of(attr, struct blk_mq_ctx_sysfs_entry, attr);
	ctx = container_of(kobj, struct blk_mq_ctx, kobj);
	q = ctx->queue;

	if (!entry->store)
		return -EIO;

	res = -ENOENT;
	mutex_lock(&q->sysfs_lock);
	if (!blk_queue_dying(q))
		res = entry->store(ctx, page, length);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static ssize_t blk_mq_hw_sysfs_show(struct kobject *kobj,
				    struct attribute *attr, char *page)
{
	struct blk_mq_hw_ctx_sysfs_entry *entry;
	struct blk_mq_hw_ctx *hctx;
	struct request_queue *q;
	ssize_t res;

	entry = container_of(attr, struct blk_mq_hw_ctx_sysfs_entry, attr);
	hctx = container_of(kobj, struct blk_mq_hw_ctx, kobj);
	q = hctx->queue;

	if (!entry->show)
		return -EIO;

	res = -ENOENT;
	mutex_lock(&q->sysfs_lock);
	if (!blk_queue_dying(q))
		res = entry->show(hctx, page);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static ssize_t blk_mq_hw_sysfs_store(struct kobject *kobj,
				     struct attribute *attr, const char *page,
				     size_t length)
{
	struct blk_mq_hw_ctx_sysfs_entry *entry;
	struct blk_mq_hw_ctx *hctx;
	struct request_queue *q;
	ssize_t res;

	entry = container_of(attr, struct blk_mq_hw_ctx_sysfs_entry, attr);
	hctx = container_of(kobj, struct blk_mq_hw_ctx, kobj);
	q = hctx->queue;

	if (!entry->store)
		return -EIO;

	res = -ENOENT;
	mutex_lock(&q->sysfs_lock);
	if (!blk_queue_dying(q))
		res = entry->store(hctx, page, length);
	mutex_unlock(&q->sysfs_lock);
	return res;
}

static ssize_t blk_mq_sysfs_dispatched_show(struct blk_mq_ctx *ctx, char *page)
{
	return sprintf(page, "%lu %lu\n", ctx->rq_dispatched[1],
				ctx->rq_dispatched[0]);
}

static ssize_t blk_mq_sysfs_merged_show(struct blk_mq_ctx *ctx, char *page)
{
	return sprintf(page, "%lu\n", ctx->rq_merged);
}

static ssize_t blk_mq_sysfs_completed_show(struct blk_mq_ctx *ctx, char *page)
{
	return sprintf(page, "%lu %lu\n", ctx->rq_completed[1],
				ctx->rq_completed[0]);
}

static ssize_t sysfs_list_show(char *page, struct list_head *list, char *msg)
{
	struct request *rq;
	int len = snprintf(page, PAGE_SIZE - 1, "%s:\n", msg);

	list_for_each_entry(rq, list, queuelist) {
		const int rq_len = 2 * sizeof(rq) + 2;

		/* if the output will be truncated */
		if (PAGE_SIZE - 1 < len + rq_len) {
			/* backspacing if it can't hold '\t...\n' */
			if (PAGE_SIZE - 1 < len + 5)
				len -= rq_len;
			len += snprintf(page + len, PAGE_SIZE - 1 - len,
					"\t...\n");
			break;
		}
		len += snprintf(page + len, PAGE_SIZE - 1 - len,
				"\t%p\n", rq);
	}

	return len;
}

static ssize_t blk_mq_sysfs_rq_list_show(struct blk_mq_ctx *ctx, char *page)
{
	ssize_t ret;

	spin_lock(&ctx->lock);
	ret = sysfs_list_show(page, &ctx->rq_list, "CTX pending");
	spin_unlock(&ctx->lock);

	return ret;
}

static ssize_t blk_mq_hw_sysfs_poll_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	return sprintf(page, "invoked=%lu, success=%lu\n", hctx->poll_invoked, hctx->poll_success);
}

static ssize_t blk_mq_hw_sysfs_queued_show(struct blk_mq_hw_ctx *hctx,
					   char *page)
{
	return sprintf(page, "%lu\n", hctx->queued);
}

static ssize_t blk_mq_hw_sysfs_run_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	return sprintf(page, "%lu\n", hctx->run);
}

static ssize_t blk_mq_hw_sysfs_dispatched_show(struct blk_mq_hw_ctx *hctx,
					       char *page)
{
	char *start_page = page;
	int i;

	page += sprintf(page, "%8u\t%lu\n", 0U, hctx->dispatched[0]);

	for (i = 1; i < BLK_MQ_MAX_DISPATCH_ORDER; i++) {
		unsigned long d = 1U << (i - 1);

		page += sprintf(page, "%8lu\t%lu\n", d, hctx->dispatched[i]);
	}

	return page - start_page;
}

static ssize_t blk_mq_hw_sysfs_rq_list_show(struct blk_mq_hw_ctx *hctx,
					    char *page)
{
	ssize_t ret;

	spin_lock(&hctx->lock);
	ret = sysfs_list_show(page, &hctx->dispatch, "HCTX pending");
	spin_unlock(&hctx->lock);

	return ret;
}

static ssize_t blk_mq_hw_sysfs_tags_show(struct blk_mq_hw_ctx *hctx, char *page)
{
#ifdef CONFIG_HISI_BLK_MQ
	return hisi_blk_mq_tag_sysfs_show(hctx->queue,hctx->tags, page);
#else
	return blk_mq_tag_sysfs_show(hctx->tags, page);
#endif
}

static ssize_t blk_mq_hw_sysfs_active_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	return sprintf(page, "%u\n", atomic_read(&hctx->nr_active));
}

static ssize_t blk_mq_hw_sysfs_cpus_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	const size_t size = PAGE_SIZE - 1;
	unsigned int i, first = 1;
	int ret = 0, pos = 0;

	for_each_cpu(i, hctx->cpumask) {
		if (first)
			ret = snprintf(pos + page, size - pos, "%u", i);
		else
			ret = snprintf(pos + page, size - pos, ", %u", i);

		if (ret >= size - pos)
			break;

		first = 0;
		pos += ret;
	}

	ret = snprintf(pos + page, size - pos, "\n");
	return pos + ret;
}

#ifdef CONFIG_WBT
static void blk_mq_stat_clear(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx;
	unsigned int i;

	hctx_for_each_ctx(hctx, ctx, i) {
		blk_stat_init(&ctx->stat[0]);
		blk_stat_init(&ctx->stat[1]);
		blk_stat_init(&ctx->stat[2]);
		blk_stat_init(&ctx->stat[3]);
	}
}

/*lint -save -e713 -e715*/
static ssize_t blk_mq_hw_sysfs_stat_store(struct blk_mq_hw_ctx *hctx,
					  const char *page, size_t count)
{
	blk_mq_stat_clear(hctx);
	return count;
}
/*lint -restore*/

static ssize_t print_stat(char *page, struct blk_rq_stat *stat, const char *pre)
{
	/*lint -save -e421*/
	return sprintf(page, "%s samples=%llu, mean=%lld, min=%lld, max=%lld\n",
			pre, (long long) stat->nr_samples,
			(long long) stat->mean, (long long) stat->min,
			(long long) stat->max);
	/*lint -restore*/
}

static ssize_t blk_mq_hw_sysfs_stat_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	struct blk_rq_stat stat[4];
	ssize_t ret;

	blk_stat_init(&stat[0]);
	blk_stat_init(&stat[1]);
	blk_stat_init(&stat[2]);
	blk_stat_init(&stat[3]);

	blk_hctx_stat_get(hctx, stat);

	ret = print_stat(page, &stat[0], "read :");
	ret += print_stat(page + ret, &stat[1], "write:");
	ret += print_stat(page + ret, &stat[2], "fg-read:");
	ret += print_stat(page + ret, &stat[3], "fg-write:");
	return ret;
}
#endif

static struct blk_mq_ctx_sysfs_entry blk_mq_sysfs_dispatched = {
	.attr = {.name = "dispatched", .mode = S_IRUGO },
	.show = blk_mq_sysfs_dispatched_show,
};
static struct blk_mq_ctx_sysfs_entry blk_mq_sysfs_merged = {
	.attr = {.name = "merged", .mode = S_IRUGO },
	.show = blk_mq_sysfs_merged_show,
};
static struct blk_mq_ctx_sysfs_entry blk_mq_sysfs_completed = {
	.attr = {.name = "completed", .mode = S_IRUGO },
	.show = blk_mq_sysfs_completed_show,
};
static struct blk_mq_ctx_sysfs_entry blk_mq_sysfs_rq_list = {
	.attr = {.name = "rq_list", .mode = S_IRUGO },
	.show = blk_mq_sysfs_rq_list_show,
};

static struct attribute *default_ctx_attrs[] = {
	&blk_mq_sysfs_dispatched.attr,
	&blk_mq_sysfs_merged.attr,
	&blk_mq_sysfs_completed.attr,
	&blk_mq_sysfs_rq_list.attr,
	NULL,
};

static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_queued = {
	.attr = {.name = "queued", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_queued_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_run = {
	.attr = {.name = "run", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_run_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_dispatched = {
	.attr = {.name = "dispatched", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_dispatched_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_active = {
	.attr = {.name = "active", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_active_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_pending = {
	.attr = {.name = "pending", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_rq_list_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_tags = {
	.attr = {.name = "tags", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_tags_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_cpus = {
	.attr = {.name = "cpu_list", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_cpus_show,
};
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_poll = {
	.attr = {.name = "io_poll", .mode = S_IRUGO },
	.show = blk_mq_hw_sysfs_poll_show,
};

#ifdef CONFIG_WBT
static struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_stat = {
	.attr = {.name = "stats", .mode = S_IRUGO | S_IWUSR },
	.show = blk_mq_hw_sysfs_stat_show,
	.store = blk_mq_hw_sysfs_stat_store,
};
#endif

static struct attribute *default_hw_ctx_attrs[] = {
	&blk_mq_hw_sysfs_queued.attr,
	&blk_mq_hw_sysfs_run.attr,
	&blk_mq_hw_sysfs_dispatched.attr,
	&blk_mq_hw_sysfs_pending.attr,
	&blk_mq_hw_sysfs_tags.attr,
	&blk_mq_hw_sysfs_cpus.attr,
	&blk_mq_hw_sysfs_active.attr,
	&blk_mq_hw_sysfs_poll.attr,
#ifdef CONFIG_WBT
	&blk_mq_hw_sysfs_stat.attr,
#endif
	NULL,
};

static const struct sysfs_ops blk_mq_sysfs_ops = {
	.show	= blk_mq_sysfs_show,
	.store	= blk_mq_sysfs_store,
};

static const struct sysfs_ops blk_mq_hw_sysfs_ops = {
	.show	= blk_mq_hw_sysfs_show,
	.store	= blk_mq_hw_sysfs_store,
};

static struct kobj_type blk_mq_ktype = {
	.sysfs_ops	= &blk_mq_sysfs_ops,
	.release	= blk_mq_sysfs_release,
};

static struct kobj_type blk_mq_ctx_ktype = {
	.sysfs_ops	= &blk_mq_sysfs_ops,
	.default_attrs	= default_ctx_attrs,
	.release	= blk_mq_sysfs_release,
};

static struct kobj_type blk_mq_hw_ktype = {
	.sysfs_ops	= &blk_mq_hw_sysfs_ops,
	.default_attrs	= default_hw_ctx_attrs,
	.release	= blk_mq_sysfs_release,
};

static void blk_mq_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx;
	int i;

	if (!hctx->nr_ctx)
		return;

	hctx_for_each_ctx(hctx, ctx, i)
		kobject_del(&ctx->kobj);

	kobject_del(&hctx->kobj);
}

static int blk_mq_register_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct blk_mq_ctx *ctx;
	int i, ret;

	if (!hctx->nr_ctx)
		return 0;

	ret = kobject_add(&hctx->kobj, &q->mq_kobj, "%u", hctx->queue_num);
	if (ret)
		return ret;

	hctx_for_each_ctx(hctx, ctx, i) {
		ret = kobject_add(&ctx->kobj, &hctx->kobj, "cpu%u", ctx->cpu);
		if (ret)
			break;
	}

	return ret;
}

void blk_mq_unregister_disk(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;
	int i, j;

	blk_mq_disable_hotplug();

	queue_for_each_hw_ctx(q, hctx, i) {
		blk_mq_unregister_hctx(hctx);

		hctx_for_each_ctx(hctx, ctx, j)
			kobject_put(&ctx->kobj);

		kobject_put(&hctx->kobj);
	}

	kobject_uevent(&q->mq_kobj, KOBJ_REMOVE);
	kobject_del(&q->mq_kobj);
	kobject_put(&q->mq_kobj);

	kobject_put(&disk_to_dev(disk)->kobj);

	q->mq_sysfs_init_done = false;
	blk_mq_enable_hotplug();
}

static void blk_mq_sysfs_init(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;
	int i;

	kobject_init(&q->mq_kobj, &blk_mq_ktype);

	queue_for_each_hw_ctx(q, hctx, i)
		kobject_init(&hctx->kobj, &blk_mq_hw_ktype);

	queue_for_each_ctx(q, ctx, i)
		kobject_init(&ctx->kobj, &blk_mq_ctx_ktype);
}

int blk_mq_register_disk(struct gendisk *disk)
{
	struct device *dev = disk_to_dev(disk);
	struct request_queue *q = disk->queue;
	struct blk_mq_hw_ctx *hctx;
	int ret, i;

	blk_mq_disable_hotplug();

	blk_mq_sysfs_init(q);

	ret = kobject_add(&q->mq_kobj, kobject_get(&dev->kobj), "%s", "mq");
	if (ret < 0)
		goto out;

	kobject_uevent(&q->mq_kobj, KOBJ_ADD);

	queue_for_each_hw_ctx(q, hctx, i) {
		ret = blk_mq_register_hctx(hctx);
		if (ret)
			break;
	}

	if (ret)
		blk_mq_unregister_disk(disk);
	else
		q->mq_sysfs_init_done = true;
out:
	blk_mq_enable_hotplug();

	return ret;
}
EXPORT_SYMBOL_GPL(blk_mq_register_disk);

void blk_mq_sysfs_unregister(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	if (!q->mq_sysfs_init_done)
		return;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_unregister_hctx(hctx);
}

int blk_mq_sysfs_register(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	int i, ret = 0;

	if (!q->mq_sysfs_init_done)
		return ret;

	queue_for_each_hw_ctx(q, hctx, i) {
		ret = blk_mq_register_hctx(hctx);
		if (ret)
			break;
	}

	return ret;
}

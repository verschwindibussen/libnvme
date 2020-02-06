#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <systemd/sd-id128.h>

#include "fabrics.h"
#include "types.h"
#include "cmd.h"
#include "util.h"

#define NVMF_HOSTID_SIZE	36
#define NVME_HOSTNQN_ID SD_ID128_MAKE(c7,f4,61,81,12,be,49,32,8c,83,10,6f,9d,dd,d8,6b)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof (*x))

const char *nvmf_dev = "/dev/nvme-fabrics";
const char *nvmf_hostnqn_file = "/etc/nvme/hostnqn";
const char *nvmf_hostid_file = "/etc/nvme/hostid";

static int add_bool_argument(char **argstr, char *tok, bool arg)
{
	char *nstr;

	if (arg) {
		if (asprintf(&nstr, "%s,%s", *argstr, tok) < 0) {
			errno = ENOMEM;
			return -1;
		}
		free(*argstr);
		*argstr = nstr;
	}
	return 0;
}

static int add_int_argument(char **argstr, char *tok, int arg,
		 bool allow_zero)
{
	char *nstr;

	if ((arg && !allow_zero) || (arg != -1 && allow_zero)) {
		if (asprintf(&nstr, "%s,%s=%d", *argstr, tok, arg) < 0) {
			errno = ENOMEM;
			return -1;
		}
		free(*argstr);
		*argstr = nstr;
	}
	return 0;
}

static int add_argument(char **argstr, const char *tok, const char *arg)
{
	char *nstr;

	if (arg && strcmp(arg, "none")) {
		if (asprintf(&nstr, "%s,%s=%s", *argstr, tok, arg) < 0) {
			errno = ENOMEM;
			return -1;
		}
		free(*argstr);
		*argstr = nstr;
	}
	return 0;
}

static int build_options(char **argstr, struct nvme_fabrics_config *cfg)
{
	/* always specify nqn as first arg - this will init the string */
	if (asprintf(argstr, "nqn=%s", cfg->nqn) < 0) {
		errno = ENOMEM;
		return -1;
	}


	if (add_argument(argstr, "transport", cfg->transport) ||
	    add_argument(argstr, "traddr", cfg->traddr) ||
	    add_argument(argstr, "host_traddr", cfg->host_traddr) ||
	    add_argument(argstr, "trsvcid", cfg->trsvcid) ||
	    add_argument(argstr, "hostnqn", cfg->hostnqn) ||
	    add_argument(argstr, "hostid", cfg->hostid) ||
	    add_int_argument(argstr, "nr_write_queues", cfg->nr_write_queues, false) ||
	    add_int_argument(argstr, "nr_poll_queues", cfg->nr_poll_queues, false) ||
	    add_int_argument(argstr, "reconnect_delay", cfg->reconnect_delay, false) ||
	    add_int_argument(argstr, "ctrl_loss_tmo", cfg->ctrl_loss_tmo, false) ||
	    add_int_argument(argstr, "tos", cfg->tos, true) ||
	    add_bool_argument(argstr, "duplicate_connect", cfg->duplicate_connect) ||
	    add_bool_argument(argstr, "disable_sqflow", cfg->disable_sqflow) ||
	    add_bool_argument(argstr, "hdr_digest", cfg->hdr_digest) ||
	    add_bool_argument(argstr, "data_digest", cfg->data_digest) ||
	    add_int_argument(argstr, "queue_size", cfg->queue_size, false) ||
	    add_int_argument(argstr, "keep_alive_tmo", cfg->keep_alive_tmo, false) ||
	    add_int_argument(argstr, "nr_io_queues", cfg->nr_io_queues, false)) {
		free(*argstr);
		return -1;
	}

	return 0;
}
static int __nvmf_add_ctrl(const char *argstr)
{
	int ret, fd, len = strlen(argstr);
	char buf[0x1000], *options, *p;

	fd = open(nvmf_dev, O_RDWR);
	if (fd < 0)
		return -1;

	ret = write(fd, argstr, len);
	if (ret != len) {
		ret = -1;
		goto out_close;
	}

	len = read(fd, buf, sizeof(buf));
	if (len < 0) {
		ret = -1;
		goto out_close;
	}

	buf[len] = '\0';
	options = buf;
	while ((p = strsep(&options, ",\n")) != NULL) {
		if (!*p)
			continue;
		if (sscanf(p, "instance=%d", &ret) == 1)
			goto out_close;
	}

	errno = EINVAL;
	ret = -1;
out_close:
	close(fd);
	return ret;
}

int nvmf_add_ctrl_opts(struct nvme_fabrics_config *cfg)
{
	char *argstr;
	int ret;

	ret = build_options(&argstr, cfg);
	if (ret)
		return ret;

	ret = __nvmf_add_ctrl(argstr);
	printf("ctrl:%s ret:%d\n", argstr, ret);

	free(argstr);
	return ret;
}

nvme_ctrl_t nvmf_add_ctrl(struct nvme_fabrics_config *cfg)
{
	char d[32];
	int ret;

	ret = nvmf_add_ctrl_opts(cfg);
	if (ret < 0)
		return NULL;

	memset(d, 0, sizeof(d));
	if (snprintf(d, sizeof(d), "nvme%d", ret) < 0)
		return NULL;

	return nvme_scan_ctrl(d);
}

static void chomp(char *s, int l)
{
	while (l && (s[l] == '\0' || s[l] == ' '))
		s[l--] = '\0';
}

nvme_ctrl_t nvmf_connect_disc_entry(struct nvmf_disc_log_entry *e,
	const struct nvme_fabrics_config *defcfg, bool *discover)
{
	struct nvme_fabrics_config cfg = { 0 };
	nvme_ctrl_t c;

	memcpy(&cfg, defcfg, sizeof(cfg));
	switch (e->subtype) {
	case NVME_NQN_DISC:
		if (discover)
			*discover = true;
		break;
	case NVME_NQN_NVME:
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	switch (e->trtype) {
	case NVMF_TRTYPE_RDMA:
	case NVMF_TRTYPE_TCP:
		switch (e->adrfam) {
		case NVMF_ADDR_FAMILY_IP4:
		case NVMF_ADDR_FAMILY_IP6:
			chomp(e->traddr, NVMF_TRADDR_SIZE);
			chomp(e->trsvcid, NVMF_TRSVCID_SIZE);
			cfg.traddr = e->traddr;
			cfg.trsvcid = e->trsvcid;
			break;
		default:
			errno = EINVAL;
			return NULL;
		}
		break;
        case NVMF_TRTYPE_FC:
		switch (e->adrfam) {
		case NVMF_ADDR_FAMILY_FC:
			chomp(e->traddr, NVMF_TRADDR_SIZE),
			cfg.traddr = e->traddr;
			cfg.trsvcid = NULL;
			break;
		}
	default:
		errno = EINVAL;
		return NULL;
	}
	cfg.transport = nvmf_trtype_str(e->trtype);

	cfg.nqn = e->subnqn;
	if (e->treq & NVMF_TREQ_DISABLE_SQFLOW)
		cfg.disable_sqflow = true;

	c = nvmf_add_ctrl(&cfg);
	if (!c && errno == EINVAL && cfg.disable_sqflow) {
		errno = 0;
		/* disable_sqflow is unrecognized option on older kernels */
		cfg.disable_sqflow = false;
		c = nvmf_add_ctrl(&cfg);
	}

	return c;
}

static int nvme_discovery_log(int fd, __u32 len, struct nvmf_discovery_log *log)
{
	return nvme_get_log_page(fd, 0, NVME_LOG_LID_DISC, true, len, log);
}

int nvmf_get_discovery_log(nvme_ctrl_t c, struct nvmf_discovery_log **logp,
	int max_retries)
{
	struct nvmf_discovery_log *log;
	int hdr, ret, retries = 0;
	uint64_t genctr, numrec;
	unsigned int size;

	hdr = sizeof(struct nvmf_discovery_log);
	log = malloc(hdr);
	if (!log) {
		errno = ENOMEM;
		return -1;
	}
	memset(log, 0, hdr);

	ret = nvme_discovery_log(nvme_ctrl_get_fd(c), 0x100, log);
	if (ret)
		goto out_free_log;

	do {
		numrec = le64_to_cpu(log->numrec);
		genctr = le64_to_cpu(log->genctr);

		free(log);
		if (numrec == 0) {
			*logp = log;
			return 0;
		}

		size = sizeof(struct nvmf_discovery_log) +
			sizeof(struct nvmf_disc_log_entry) * (numrec);

		log = malloc(size);
		if (!log) {
			errno = ENOMEM;
			return -1;
		}
		memset(log, 0, size);

		ret = nvme_discovery_log(nvme_ctrl_get_fd(c), size, log);
		if (ret)
			goto out_free_log;

		genctr = le64_to_cpu(log->genctr);
		ret = nvme_discovery_log(nvme_ctrl_get_fd(c), hdr, log);
		if (ret)
			goto out_free_log;
	} while (genctr != le64_to_cpu(log->genctr) &&
		 ++retries < max_retries);

	if (genctr != le64_to_cpu(log->genctr)) {
		errno = EAGAIN;
		ret = -1;
	} else if (numrec != le64_to_cpu(log->numrec)) {
		errno = EBADSLT;
		ret = -1;
	} else {
		*logp = log;
		return 0;
	}

out_free_log:
	free(log);
	return ret;
}

char *nvmf_hostnqn_generate()
{
	sd_id128_t id;
	char *ret = NULL;

	if (sd_id128_get_machine_app_specific(NVME_HOSTNQN_ID, &id) < 0)
		return NULL;

	if (asprintf(&ret,
		     "nqn.2014-08.org.nvmexpress:uuid:" SD_ID128_FORMAT_STR "\n",
		     SD_ID128_FORMAT_VAL(id)) < 0)
		ret = NULL;

	return ret;
}

static char *nvmf_read_file(const char *f, int len)
{
	char buf[len];
	int ret, fd;

	fd = open(f, O_RDONLY);
	if (fd < 0)
		return false;

	memset(buf, 0, len);
	ret = read(fd, buf, sizeof(buf - 1));
	close (fd);

	if (ret < 0)
		return NULL;
	return strndup(buf, strcspn(buf, "\n"));
}

char *nvmf_hostnqn_from_file()
{
	return nvmf_read_file(nvmf_hostnqn_file, NVMF_NQN_SIZE);
}

char *nvmf_hostid_from_file()
{
	return nvmf_read_file(nvmf_hostid_file, NVMF_HOSTID_SIZE);
}

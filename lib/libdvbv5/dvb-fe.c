/*
 * Copyright (c) 2011-2012 - Mauro Carvalho Chehab <mchehab@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * Or, point your browser to http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */
#include <sys/types.h>

#include "dvb-v5.h"
#include "dvb-v5-std.h"
#include "dvb-fe.h"

#include <stddef.h>
#include <unistd.h>

static void dvb_v5_free(struct dvb_v5_fe_parms *parms)
{
	if (parms->fname)
		free(parms->fname);

	free(parms);
}

struct dvb_v5_fe_parms *dvb_fe_open(int adapter, int frontend, unsigned verbose,
				    unsigned use_legacy_call)
{
  return dvb_fe_open2(adapter, frontend, verbose, use_legacy_call,
                      dvb_default_log);
}

struct dvb_v5_fe_parms *dvb_fe_open2(int adapter, int frontend, unsigned verbose,
				    unsigned use_legacy_call, dvb_logfunc logfunc)
{
	int fd, i;
	char *fname;
	struct dtv_properties dtv_prop;
	struct dvb_v5_fe_parms *parms = NULL;

	asprintf(&fname, "/dev/dvb/adapter%i/frontend%i", adapter, frontend);
	if (!fname) {
		logfunc(LOG_ERR, "fname calloc: %s", strerror(errno));
		return NULL;
	}

	fd = open(fname, O_RDWR, 0);
	if (fd == -1) {
		logfunc(LOG_ERR, "%s while opening %s", strerror(errno), fname);
		return NULL;
	}
	parms = calloc(sizeof(*parms), 1);
	if (!parms) {
		logfunc(LOG_ERR, "parms calloc: %s", strerror(errno));
		close(fd);
		return NULL;
	}
	parms->fname = fname;
	parms->verbose = verbose;
	parms->fd = fd;
	parms->sat_number = -1;
        parms->abort = 0;
        parms->logfunc = logfunc;

	if (ioctl(fd, FE_GET_INFO, &parms->info) == -1) {
		dvb_perror("FE_GET_INFO");
		dvb_v5_free(parms);
		close(fd);
		return NULL;
	}

	if (verbose) {
		fe_caps_t caps = parms->info.caps;

		dvb_log("Device %s (%s) capabilities:",
			parms->info.name, fname);
		for (i = 0; i < ARRAY_SIZE(fe_caps_name); i++) {
			if (caps & fe_caps_name[i].idx)
				dvb_log ("     %s", fe_caps_name[i].name);
		}
	}

	parms->dvb_prop[0].cmd = DTV_API_VERSION;
	parms->dvb_prop[1].cmd = DTV_DELIVERY_SYSTEM;

	dtv_prop.num = 2;
	dtv_prop.props = parms->dvb_prop;

	/* Detect a DVBv3 device */
	if (ioctl(fd, FE_GET_PROPERTY, &dtv_prop) == -1) {
		parms->dvb_prop[0].u.data = 0x300;
		parms->dvb_prop[1].u.data = SYS_UNDEFINED;
	}
	parms->version = parms->dvb_prop[0].u.data;
	parms->current_sys = parms->dvb_prop[1].u.data;
	if (verbose)
		dvb_log ("DVB API Version %d.%d%s, Current v5 delivery system: %s",
			parms->version / 256,
			parms->version % 256,
			use_legacy_call ? " (forcing DVBv3 calls)" : "",
			delivery_system_name[parms->current_sys]);

	if (parms->version < 0x500)
		use_legacy_call = 1;

	if (use_legacy_call || parms->version < 0x505) {
		parms->legacy_fe = 1;
		switch(parms->info.type) {
		case FE_QPSK:
			parms->current_sys = SYS_DVBS;
			parms->systems[parms->num_systems++] = parms->current_sys;
			if (parms->version < 0x0500)
				break;
			if (parms->info.caps & FE_CAN_2G_MODULATION)
				parms->systems[parms->num_systems++] = SYS_DVBS2;
			if (parms->info.caps & FE_CAN_TURBO_FEC)
				parms->systems[parms->num_systems++] = SYS_TURBO;
			break;
		case FE_QAM:
			parms->current_sys = SYS_DVBC_ANNEX_A;
			parms->systems[parms->num_systems++] = parms->current_sys;
			break;
		case FE_OFDM:
			parms->current_sys = SYS_DVBT;
			parms->systems[parms->num_systems++] = parms->current_sys;
			if (parms->version < 0x0500)
				break;
			if (parms->info.caps & FE_CAN_2G_MODULATION)
				parms->systems[parms->num_systems++] = SYS_DVBT2;
			break;
		case FE_ATSC:
			if (parms->info.caps & (FE_CAN_8VSB | FE_CAN_16VSB))
				parms->systems[parms->num_systems++] = SYS_ATSC;
			if (parms->info.caps & (FE_CAN_QAM_64 | FE_CAN_QAM_256 | FE_CAN_QAM_AUTO))
				parms->systems[parms->num_systems++] = SYS_DVBC_ANNEX_B;
			parms->current_sys = parms->systems[0];
			break;
		}
		if (!parms->num_systems) {
			dvb_logerr("delivery system not detected");
			dvb_v5_free(parms);
			close(fd);
			return NULL;
		}
	} else {
		parms->dvb_prop[0].cmd = DTV_ENUM_DELSYS;
		parms->n_props = 1;
		dtv_prop.num = 1;
		dtv_prop.props = parms->dvb_prop;
		if (ioctl(fd, FE_GET_PROPERTY, &dtv_prop) == -1) {
			dvb_perror("FE_GET_PROPERTY");
			dvb_v5_free(parms);
			close(fd);
			return NULL;
		}
		parms->num_systems = parms->dvb_prop[0].u.buffer.len;
		for (i = 0; i < parms->num_systems; i++)
			parms->systems[i] = parms->dvb_prop[0].u.buffer.data[i];

		if (parms->num_systems == 0) {
			dvb_logerr("driver died while trying to set the delivery system");
			dvb_v5_free(parms);
			close(fd);
			return NULL;
		}
	}

	if (verbose) {
		dvb_log("Supported delivery system%s: ",
		       (parms->num_systems > 1) ? "s" : "");
		for (i = 0; i < parms->num_systems; i++) {
			if (parms->systems[i] == parms->current_sys)
				dvb_log ("    [%s]",
					delivery_system_name[parms->systems[i]]);
			else
				dvb_log ("     %s",
					delivery_system_name[parms->systems[i]]);
		}
		if (use_legacy_call || parms->version < 0x505)
			dvb_log("Warning: new delivery systems like ISDB-T, ISDB-S, DMB-TH, DSS, ATSC-MH will be miss-detected by a DVBv5.4 or earlier API call");
	}

	/*
	 * Fix a bug at some DVB drivers
	 */
	if (parms->current_sys == SYS_UNDEFINED)
		parms->current_sys = parms->systems[0];

	/* Prepare to use the delivery system */
	dvb_set_sys(parms, parms->current_sys);

	/* Prepare the status struct */
	parms->stats.prop[0].cmd = DTV_STATUS;
	parms->stats.prop[1].cmd = DTV_BER;
	parms->stats.prop[2].cmd = DTV_SIGNAL_STRENGTH;
	parms->stats.prop[3].cmd = DTV_SNR;
	parms->stats.prop[4].cmd = DTV_UNCORRECTED_BLOCKS;

	return parms;
}


static int is_satellite(uint32_t delivery_system)
{
	switch (delivery_system) {
	case SYS_DVBS:
	case SYS_DVBS2:
	case SYS_TURBO:
	case SYS_ISDBS:
		return 1;
	default:
		return 0;

	}
}


void dvb_fe_close(struct dvb_v5_fe_parms *parms)
{
	if (!parms)
		return;

	if (parms->fd < 0)
		return;

	/* Disable LNBf power */
	if (is_satellite(parms->current_sys))
		dvb_fe_sec_voltage(parms, 0, 0);

	close(parms->fd);

	dvb_v5_free(parms);
}


int dvb_add_parms_for_sys(struct dtv_property *dvb_prop,
			  unsigned max_size,
			  fe_delivery_system_t sys)
{
	const unsigned int *sys_props;
	int n;

	/* Make dvb properties reflect the current standard */

	sys_props = dvb_v5_delivery_system[sys];
	if (!sys_props)
		return EINVAL;

	n = 0;
	while (sys_props[n]) {
		dvb_prop[n].cmd = sys_props[n];
		dvb_prop[n].u.data = 0;
		n++;
	}
	dvb_prop[n].cmd = DTV_DELIVERY_SYSTEM;
	dvb_prop[n].u.data = sys;
	n++;

	return n;
}

int dvb_set_sys(struct dvb_v5_fe_parms *parms,
			  fe_delivery_system_t sys)
{
	struct dtv_property dvb_prop[1];
	struct dtv_properties prop;
	int rc;

	if (sys != parms->current_sys) {
		/* Disable LNBf power */
		if (is_satellite(parms->current_sys) &&
		    !is_satellite(sys))
			dvb_fe_sec_voltage(parms, 0, 0);

		/* Can't change standard with the legacy FE support */
		if (parms->legacy_fe)
			return EINVAL;

		dvb_prop[0].cmd = DTV_DELIVERY_SYSTEM;
		dvb_prop[0].u.data = sys;
		prop.num = 1;
		prop.props = dvb_prop;

		if (ioctl(parms->fd, FE_SET_PROPERTY, &prop) == -1) {
			dvb_perror("Set delivery system");
			return errno;
		}
	}

	rc = dvb_add_parms_for_sys(parms->dvb_prop,
				   ARRAY_SIZE(parms->dvb_prop), sys);
	if (rc < 0)
		return EINVAL;

	parms->current_sys = sys;
	parms->n_props = rc;

	return 0;
}

static enum dvbv3_emulation_type dvbv3_type(uint32_t delivery_system)
{
	switch (delivery_system) {
	case SYS_DVBC_ANNEX_A:
	case SYS_DVBC_ANNEX_C:
		return DVBV3_QAM;
	case SYS_DVBS:
	case SYS_DVBS2:
	case SYS_TURBO:
	case SYS_ISDBS:
	case SYS_DSS:
		return DVBV3_QPSK;
	case SYS_DVBT:
	case SYS_DVBT2:
	case SYS_ISDBT:
	case SYS_DMBTH:
		return DVBV3_OFDM;
	case SYS_ATSC:
	case SYS_DVBC_ANNEX_B:
		return DVBV3_ATSC;
	default:
		return DVBV3_UNKNOWN;
	}
};

static int is_dvbv3_delsys(uint32_t delsys)
{
	int status;

	status = (delsys == SYS_DVBT) || (delsys == SYS_DVBC_ANNEX_A) ||
		 (delsys == SYS_DVBS) || (delsys == SYS_ATSC);

	return status;
}

int dvb_set_compat_delivery_system(struct dvb_v5_fe_parms *parms,
				   uint32_t desired_system)
{
	int i;
	uint32_t delsys = SYS_UNDEFINED;
	enum dvbv3_emulation_type type;


	/* Check if the desired delivery system is supported */
	for (i = 0; i < parms->num_systems; i++) {
		if (parms->systems[i] == desired_system) {
			dvb_set_sys(parms, desired_system);
			return 0;
		}
	}

	/*
	 * Find the closest DVBv3 system that matches the delivery
	 * system.
	 */
	type = dvbv3_type(desired_system);

	/*
	 * Get the last non-DVBv3 delivery system that has the same type
	 * of the desired system
	 */
	for (i = 0; i < parms->num_systems; i++) {
		if ((dvbv3_type(parms->systems[i]) == type) &&
		    !is_dvbv3_delsys(parms->systems[i]))
			delsys = parms->systems[i];
	}

	if (delsys == SYS_UNDEFINED)
		return -1;

	dvb_set_sys(parms, desired_system);

	/* Put ISDB-T into auto mode */
	if (desired_system == SYS_ISDBT) {
		dvb_fe_store_parm(parms, DTV_BANDWIDTH_HZ, 6000000);
		dvb_fe_store_parm(parms, DTV_ISDBT_PARTIAL_RECEPTION, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_SOUND_BROADCASTING, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_SB_SUBCHANNEL_ID, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_SB_SEGMENT_IDX, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_SB_SEGMENT_COUNT, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYER_ENABLED, 7);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERA_FEC, FEC_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERB_FEC, FEC_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERC_FEC, FEC_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERA_MODULATION, QAM_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERB_MODULATION, QAM_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERC_MODULATION, QAM_AUTO);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERA_SEGMENT_COUNT, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERA_TIME_INTERLEAVING, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERB_SEGMENT_COUNT, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERB_TIME_INTERLEAVING, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERC_SEGMENT_COUNT, 0);
		dvb_fe_store_parm(parms, DTV_ISDBT_LAYERC_TIME_INTERLEAVING, 0);
	}
	return 0;
}

const char *dvb_cmd_name(int cmd)
{
	if (cmd >= 0 && cmd < DTV_USER_COMMAND_START)
		return dvb_v5_name[cmd];
	else if (cmd >= 0 && cmd <= DTV_MAX_USER_COMMAND)
		return dvb_user_name[cmd - DTV_USER_COMMAND_START];
	return NULL;
}

const char *const *dvb_attr_names(int cmd)
{
	if (cmd >= 0 && cmd < DTV_USER_COMMAND_START)
		return dvb_v5_attr_names[cmd];
	else if (cmd >= 0 && cmd <= DTV_MAX_USER_COMMAND)
		return dvb_user_attr_names[cmd - DTV_USER_COMMAND_START];
	return NULL;
}

void dvb_fe_prt_parms(const struct dvb_v5_fe_parms *parms)
{
	int i;

	for (i = 0; i < parms->n_props; i++) {
		const char * const *attr_name = dvb_attr_names(parms->dvb_prop[i].cmd);
		if (attr_name) {
			int j;

			for (j = 0; j < parms->dvb_prop[i].u.data; j++) {
				if (!*attr_name)
					break;
				attr_name++;
			}
		}

		if (!attr_name || !*attr_name)
			dvb_log("%s = %u",
				dvb_cmd_name(parms->dvb_prop[i].cmd),
				parms->dvb_prop[i].u.data);
		else
			dvb_log("%s = %s",
				dvb_cmd_name(parms->dvb_prop[i].cmd),
				*attr_name);
	}
};

int dvb_fe_retrieve_parm(const struct dvb_v5_fe_parms *parms,
				unsigned cmd, uint32_t *value)
{
	int i;
	for (i = 0; i < parms->n_props; i++) {
		if (parms->dvb_prop[i].cmd != cmd)
			continue;
		*value = parms->dvb_prop[i].u.data;
		return 0;
	}
	dvb_logerr("command %s (%d) not found during retrieve",
		dvb_cmd_name(cmd), cmd);

	return EINVAL;
}

int dvb_fe_store_parm(struct dvb_v5_fe_parms *parms,
			     unsigned cmd, uint32_t value)
{
	int i;
	for (i = 0; i < parms->n_props; i++) {
		if (parms->dvb_prop[i].cmd != cmd)
			continue;
		parms->dvb_prop[i].u.data = value;
		return 0;
	}
	dvb_logerr("command %s (%d) not found during store",
		dvb_cmd_name(cmd), cmd);

	return EINVAL;
}

static int dvb_copy_fe_props(const struct dtv_property *from, int n, struct dtv_property *to)
{
	int i, j;
	for (i = 0, j = 0; i < n; i++)
		if (from[i].cmd < DTV_USER_COMMAND_START)
			to[j++] = from[i];
	return j;
}

int dvb_fe_get_parms(struct dvb_v5_fe_parms *parms)
{
	int n = 0;
	const unsigned int *sys_props;
	struct dtv_properties prop;
	struct dvb_frontend_parameters v3_parms;
	uint32_t bw;

	sys_props = dvb_v5_delivery_system[parms->current_sys];
	if (!sys_props)
		return EINVAL;

	while (sys_props[n]) {
		parms->dvb_prop[n].cmd = sys_props[n];
		n++;
	}
	parms->dvb_prop[n].cmd = DTV_DELIVERY_SYSTEM;
	parms->dvb_prop[n].u.data = parms->current_sys;
	n++;

	/* Keep it ready for set */
	parms->dvb_prop[n].cmd = DTV_TUNE;
	parms->n_props = n;

	struct dtv_property fe_prop[DTV_MAX_COMMAND];
	n = dvb_copy_fe_props(parms->dvb_prop, n, fe_prop);

	prop.props = fe_prop;
	prop.num = n;
	if (!parms->legacy_fe) {
		if (ioctl(parms->fd, FE_GET_PROPERTY, &prop) == -1) {
			dvb_perror("FE_GET_PROPERTY");
			return errno;
		}
		if (parms->verbose) {
			dvb_log("Got parameters for %s:",
			       delivery_system_name[parms->current_sys]);
			dvb_fe_prt_parms(parms);
		}
		goto ret;
	}
	/* DVBv3 call */
	if (ioctl(parms->fd, FE_GET_FRONTEND, &v3_parms) == -1) {
		dvb_perror("FE_GET_FRONTEND");
		return -1;
	}

	dvb_fe_store_parm(parms, DTV_FREQUENCY, v3_parms.frequency);
	dvb_fe_store_parm(parms, DTV_INVERSION, v3_parms.inversion);
	switch (parms->current_sys) {
	case SYS_DVBS:
		dvb_fe_store_parm(parms, DTV_SYMBOL_RATE, v3_parms.u.qpsk.symbol_rate);
		dvb_fe_store_parm(parms, DTV_INNER_FEC, v3_parms.u.qpsk.fec_inner);
		break;
	case SYS_DVBC_ANNEX_A:
		dvb_fe_store_parm(parms, DTV_SYMBOL_RATE, v3_parms.u.qam.symbol_rate);
		dvb_fe_store_parm(parms, DTV_INNER_FEC, v3_parms.u.qam.fec_inner);
		dvb_fe_store_parm(parms, DTV_MODULATION, v3_parms.u.qam.modulation);
		break;
	case SYS_ATSC:
	case SYS_DVBC_ANNEX_B:
		dvb_fe_store_parm(parms, DTV_MODULATION, v3_parms.u.vsb.modulation);
		break;
	case SYS_DVBT:
		if (v3_parms.u.ofdm.bandwidth < ARRAY_SIZE(fe_bandwidth_name) -1)
			bw = fe_bandwidth_name[v3_parms.u.ofdm.bandwidth];
		else bw = 0;
		dvb_fe_store_parm(parms, DTV_BANDWIDTH_HZ, bw);
		dvb_fe_store_parm(parms, DTV_CODE_RATE_HP, v3_parms.u.ofdm.code_rate_HP);
		dvb_fe_store_parm(parms, DTV_CODE_RATE_LP, v3_parms.u.ofdm.code_rate_LP);
		dvb_fe_store_parm(parms, DTV_MODULATION, v3_parms.u.ofdm.constellation);
		dvb_fe_store_parm(parms, DTV_TRANSMISSION_MODE, v3_parms.u.ofdm.transmission_mode);
		dvb_fe_store_parm(parms, DTV_GUARD_INTERVAL, v3_parms.u.ofdm.guard_interval);
		dvb_fe_store_parm(parms, DTV_HIERARCHY, v3_parms.u.ofdm.hierarchy_information);
		break;
	default:
		return -EINVAL;
	}

ret:
	/* For satellite, need to recover from LNBf IF frequency */
	if (is_satellite(parms->current_sys))
		return dvb_sat_get_parms(parms);

	return 0;
}

int dvb_fe_set_parms(struct dvb_v5_fe_parms *parms)
{
	struct dtv_properties prop;
	struct dvb_frontend_parameters v3_parms;
	uint32_t freq;
	uint32_t bw;

	struct dtv_property fe_prop[DTV_MAX_COMMAND];

	if (is_satellite(parms->current_sys)) {
		dvb_fe_retrieve_parm(parms, DTV_FREQUENCY, &freq);
		dvb_sat_set_parms(parms);
	}

	int n = dvb_copy_fe_props(parms->dvb_prop, parms->n_props, fe_prop);

	prop.props = fe_prop;
	prop.num = n;
	prop.props[prop.num].cmd = DTV_TUNE;
	prop.num++;

	if (!parms->legacy_fe) {
		if (ioctl(parms->fd, FE_SET_PROPERTY, &prop) == -1) {
			dvb_perror("FE_SET_PROPERTY");
			if (parms->verbose)
				dvb_fe_prt_parms(parms);
			return -1;
		}
		goto ret;
	}
	/* DVBv3 call */

	dvb_fe_retrieve_parm(parms, DTV_FREQUENCY, &v3_parms.frequency);
	dvb_fe_retrieve_parm(parms, DTV_INVERSION, &v3_parms.inversion);
	switch (parms->current_sys) {
	case SYS_DVBS:
		dvb_fe_retrieve_parm(parms, DTV_SYMBOL_RATE, &v3_parms.u.qpsk.symbol_rate);
		dvb_fe_retrieve_parm(parms, DTV_INNER_FEC, &v3_parms.u.qpsk.fec_inner);
		break;
	case SYS_DVBC_ANNEX_AC:
		dvb_fe_retrieve_parm(parms, DTV_SYMBOL_RATE, &v3_parms.u.qam.symbol_rate);
		dvb_fe_retrieve_parm(parms, DTV_INNER_FEC, &v3_parms.u.qam.fec_inner);
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &v3_parms.u.qam.modulation);
		break;
	case SYS_ATSC:
	case SYS_DVBC_ANNEX_B:
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &v3_parms.u.vsb.modulation);
		break;
	case SYS_DVBT:
		for (bw = 0; fe_bandwidth_name[bw] != 0; bw++) {
			if (fe_bandwidth_name[bw] == v3_parms.u.ofdm.bandwidth)
				break;
		}
		dvb_fe_retrieve_parm(parms, DTV_BANDWIDTH_HZ, &bw);
		dvb_fe_retrieve_parm(parms, DTV_CODE_RATE_HP, &v3_parms.u.ofdm.code_rate_HP);
		dvb_fe_retrieve_parm(parms, DTV_CODE_RATE_LP, &v3_parms.u.ofdm.code_rate_LP);
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &v3_parms.u.ofdm.constellation);
		dvb_fe_retrieve_parm(parms, DTV_TRANSMISSION_MODE, &v3_parms.u.ofdm.transmission_mode);
		dvb_fe_retrieve_parm(parms, DTV_GUARD_INTERVAL, &v3_parms.u.ofdm.guard_interval);
		dvb_fe_retrieve_parm(parms, DTV_HIERARCHY, &v3_parms.u.ofdm.hierarchy_information);
		break;
	default:
		return -EINVAL;
	}
	if (ioctl(parms->fd, FE_SET_FRONTEND, &v3_parms) == -1) {
		dvb_perror("FE_SET_FRONTEND");
		if (parms->verbose)
			dvb_fe_prt_parms(parms);
		return -1;
	}
ret:
	/* For satellite, need to recover from LNBf IF frequency */
	if (is_satellite(parms->current_sys))
		dvb_fe_store_parm(parms, DTV_FREQUENCY, freq);

	return 0;
}

int dvb_fe_retrieve_stats(struct dvb_v5_fe_parms *parms,
			   unsigned cmd, uint32_t *value)
{
	int i;
	for (i = 0; i < DTV_MAX_STATS; i++) {
		if (parms->stats.prop[i].cmd != cmd)
			continue;
		*value = parms->stats.prop[i].u.data;
		return 0;
	}
	dvb_logerr("%s not found on retrieve",
		dvb_cmd_name(cmd));

	return EINVAL;
}

int dvb_fe_store_stats(struct dvb_v5_fe_parms *parms,
			unsigned cmd, uint32_t value)
{
	int i;
	for (i = 0; i < DTV_MAX_STATS; i++) {
		if (parms->stats.prop[i].cmd != cmd)
			continue;
		parms->stats.prop[i].u.data = value;
		return 0;
	}
	dvb_logerr("%s not found on store",
		dvb_cmd_name(cmd));

	return EINVAL;
}

int dvb_fe_get_stats(struct dvb_v5_fe_parms *parms)
{
	fe_status_t status;
	uint32_t ber, ucb;
	uint16_t strength, snr;
	int i;

	if (ioctl(parms->fd, FE_READ_STATUS, &status) == -1) {
		dvb_perror("FE_READ_STATUS");
		status = -1;
	}
	dvb_fe_store_stats(parms, DTV_STATUS, status);

	if (ioctl(parms->fd, FE_READ_BER, &ber) == -1)
		ber = 0;
	dvb_fe_store_stats(parms, DTV_BER, ber);

	if (ioctl(parms->fd, FE_READ_SIGNAL_STRENGTH, &strength) == -1)
		strength = (uint16_t) -1;
	dvb_fe_store_stats(parms, DTV_SIGNAL_STRENGTH, strength);

	if (ioctl(parms->fd, FE_READ_SNR, &snr) == -1)
		snr = (uint16_t) -1;
	dvb_fe_store_stats(parms, DTV_SNR, snr);

	if (ioctl(parms->fd, FE_READ_UNCORRECTED_BLOCKS, &ucb) == -1)
		ucb = 0;
	dvb_fe_store_stats(parms, DTV_UNCORRECTED_BLOCKS, snr);


	if (parms->verbose > 1) {
		dvb_log("Status: ");
		for (i = 0; i < ARRAY_SIZE(fe_status_name); i++) {
			if (status & fe_status_name[i].idx)
				dvb_log ("    %s", fe_status_name[i].name);
		}
		dvb_log("BER: %d, Strength: %d, SNR: %d, UCB: %d",
		       ber, strength, snr, ucb);
	}
	return status;
}


int dvb_fe_get_event(struct dvb_v5_fe_parms *parms)
{
	struct dvb_frontend_event event;
	fe_status_t status;
	int i;

	if (!parms->legacy_fe) {
		dvb_fe_get_parms(parms);
		return dvb_fe_get_stats(parms);
	}

	if (ioctl(parms->fd, FE_GET_EVENT, &event) == -1) {
		dvb_perror("FE_GET_EVENT");
		return -1;
	}
	status = event.status;
	if (parms->verbose > 1) {
		dvb_log("Status: ");
		for (i = 0; i < ARRAY_SIZE(fe_status_name); i++) {
			if (status & fe_status_name[i].idx)
				dvb_log ("    %s", fe_status_name[i].name);
		}
	}
	dvb_fe_store_stats(parms, DTV_STATUS, status);

	dvb_fe_retrieve_parm(parms, DTV_FREQUENCY, &event.parameters.frequency);
	dvb_fe_retrieve_parm(parms, DTV_INVERSION, &event.parameters.inversion);
	switch (parms->current_sys) {
	case SYS_DVBS:
		dvb_fe_retrieve_parm(parms, DTV_SYMBOL_RATE, &event.parameters.u.qpsk.symbol_rate);
		dvb_fe_retrieve_parm(parms, DTV_INNER_FEC, &event.parameters.u.qpsk.fec_inner);
		break;
	case SYS_DVBC_ANNEX_AC:
		dvb_fe_retrieve_parm(parms, DTV_SYMBOL_RATE, &event.parameters.u.qam.symbol_rate);
		dvb_fe_retrieve_parm(parms, DTV_INNER_FEC, &event.parameters.u.qam.fec_inner);
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &event.parameters.u.qam.modulation);
		break;
	case SYS_ATSC:
	case SYS_DVBC_ANNEX_B:
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &event.parameters.u.vsb.modulation);
		break;
	case SYS_DVBT:
		dvb_fe_retrieve_parm(parms, DTV_BANDWIDTH_HZ, &event.parameters.u.ofdm.bandwidth);
		dvb_fe_retrieve_parm(parms, DTV_CODE_RATE_HP, &event.parameters.u.ofdm.code_rate_HP);
		dvb_fe_retrieve_parm(parms, DTV_CODE_RATE_LP, &event.parameters.u.ofdm.code_rate_LP);
		dvb_fe_retrieve_parm(parms, DTV_MODULATION, &event.parameters.u.ofdm.constellation);
		dvb_fe_retrieve_parm(parms, DTV_TRANSMISSION_MODE, &event.parameters.u.ofdm.transmission_mode);
		dvb_fe_retrieve_parm(parms, DTV_GUARD_INTERVAL, &event.parameters.u.ofdm.guard_interval);
		dvb_fe_retrieve_parm(parms, DTV_HIERARCHY, &event.parameters.u.ofdm.hierarchy_information);
		break;
	default:
		return -EINVAL;
	}

	return dvb_fe_get_stats(parms);
}

/*
 * Implement SEC/LNB/DISEqC specific functions
 * For now, DVBv5 API doesn't support those commands. So, use the DVBv3
 * version.
 */

int dvb_fe_sec_voltage(struct dvb_v5_fe_parms *parms, int on, int v18)
{
	fe_sec_voltage_t v;
	int rc;

	if (!on) {
		v = SEC_VOLTAGE_OFF;
		if (parms->verbose)
			dvb_log("DiSEqC VOLTAGE: OFF");
	} else {
		v = v18 ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
		if (parms->verbose)
			dvb_log("DiSEqC VOLTAGE: %s", v18 ? "18" : "13");
	}
	rc = ioctl(parms->fd, FE_SET_VOLTAGE, v);
	if (rc == -1)
		dvb_perror("FE_SET_VOLTAGE");
	return rc;
}

int dvb_fe_sec_tone(struct dvb_v5_fe_parms *parms, fe_sec_tone_mode_t tone)
{
	int rc;
	if (parms->verbose)
		dvb_log( "DiSEqC TONE: %s", fe_tone_name[tone] );
	rc = ioctl(parms->fd, FE_SET_TONE, tone);
	if (rc == -1)
		dvb_perror("FE_SET_TONE");
	return rc;
}

int dvb_fe_lnb_high_voltage(struct dvb_v5_fe_parms *parms, int on)
{
	int rc;

	if (on) on = 1;
	if (parms->verbose)
		dvb_log( "DiSEqC HIGH LNB VOLTAGE: %s", on ? "ON" : "OFF" );
	rc = ioctl(parms->fd, FE_ENABLE_HIGH_LNB_VOLTAGE, on);
	if (rc == -1)
		dvb_perror("FE_ENABLE_HIGH_LNB_VOLTAGE");
	return rc;
}

int dvb_fe_diseqc_burst(struct dvb_v5_fe_parms *parms, int mini_b)
{
	fe_sec_mini_cmd_t mini;
	int rc;

	mini = mini_b ? SEC_MINI_B : SEC_MINI_A;

	if (parms->verbose)
		dvb_log( "DiSEqC BURST: %s", mini_b ? "SEC_MINI_B" : "SEC_MINI_A" );
	rc = ioctl(parms->fd, FE_DISEQC_SEND_BURST, mini);
	if (rc == -1)
		dvb_perror("FE_DISEQC_SEND_BURST");
	return rc;
}

int dvb_fe_diseqc_cmd(struct dvb_v5_fe_parms *parms, const unsigned len,
		      const unsigned char *buf)
{
	struct dvb_diseqc_master_cmd msg;
	int rc;

	if (len > 6)
		return -EINVAL;

	msg.msg_len = len;
	memcpy(msg.msg, buf, len);

	if (parms->verbose) {
		int i;
		char log[len * 3 + 20], *p = log;

		p += sprintf(p, "DiSEqC command: ");
		for (i = 0; i < len; i++)
			p += sprintf (p, "%02x ", buf[i]);
		dvb_log("%s", log);
	}

	rc = ioctl(parms->fd, FE_DISEQC_SEND_MASTER_CMD, &msg);
	if (rc == -1)
		dvb_perror("FE_DISEQC_SEND_MASTER_CMD");
	return rc;
}

int dvb_fe_diseqc_reply(struct dvb_v5_fe_parms *parms, unsigned *len, char *buf,
		       int timeout)
{
	struct dvb_diseqc_slave_reply reply;
	int rc;

	if (*len > 4)
		*len = 4;

	reply.timeout = timeout;
	reply.msg_len = *len;

	if (parms->verbose)
		dvb_log("DiSEqC FE_DISEQC_RECV_SLAVE_REPLY");

	rc = ioctl(parms->fd, FE_DISEQC_RECV_SLAVE_REPLY, reply);
	if (rc == -1) {
		dvb_perror("FE_DISEQC_RECV_SLAVE_REPLY");
		return rc;
	}

	*len = reply.msg_len;
	memcpy(buf, reply.msg, reply.msg_len);

	return 0;
}

#include "btdatatype.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "bt_audio_hw.h"
#include "sysdep.h"
#include "porting_utils.h"
#include "tl.h" /* FIMXE */

#include "global.h"
#include <errno.h>
#include <pthread.h>
#include "internal_hf.h"
#include "auto_hf.h"

#include "audio_ring_buffer.h"

#include "bt_event.h"

#define MODULE_TAG "bt_audio_hw"
#include "brt_trace.h"
#define LOG(fmt, ...) bse_log_info("bt_audio_hw", fmt, ## __VA_ARGS__)

#define SCO_PKT_PREFIX_LEN 4
#define SCO_SEND_BUFFER_SIZE 256
#define SCO_SEND_DATA_SIZE (SCO_SEND_BUFFER_SIZE - SCO_PKT_PREFIX_LEN)

#define SCO_PKT_DEFAULT_SIZE 48
static unsigned char tx_sco_send_buf[SCO_SEND_DATA_SIZE];

static audio_sco_callbacks_t *s_sco_callback;
static int micmute_f = 0;
static int s_sco_pkt_len = SCO_PKT_DEFAULT_SIZE;
static unsigned char s_sco_send_buf[SCO_SEND_BUFFER_SIZE];

static int s_sco_sample = 8000;
static UINT16 s_sco_handle = 0;

static audio_a2dp_callbacks_t *s_a2dp_callback;
extern BOOL s_a2dp_mute;
static int is_paused = 0;
static int s_a2dp_sample = 44100;

#define AUDIO_DIGITAL_VOLUME_MAX 15
#define AUDIO_DIGITAL_VOLUME_DEFAULT 8

static UINT8 s_a2dp_digital_volume = AUDIO_DIGITAL_VOLUME_DEFAULT;
static UINT8 s_hfp_digital_volume = AUDIO_DIGITAL_VOLUME_DEFAULT;

#ifdef BRT_DBG
#undef BRT_DBG
#define BRT_DBG(...) ((void *) 0)
#endif

#define HAL_CBACK(P_CB, P_CBACK, ...)           \
    if (P_CB && P_CB->P_CBACK) {                \
        BRT_DBG("HAL %s->%s", #P_CB, #P_CBACK); \
        P_CB->P_CBACK(__VA_ARGS__);             \
    }                                           \
    else {                                      \
        BRT_DBG("%s", "Callback is NULL"); \
    }


static bt_play_state_callback s_play_state_cb = NULL;
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
#define AVRCP_LOGICAL_STATE_STOPPED 0
#define AVRCP_LOGICAL_STATE_PLAYING 1
#define AVRCP_LOGICAL_STATE_PAUSED  2

static BT_PLAY_STATE_E s_last_a2dp_reported_state = BT_PLAY_STATE_END;
static BOOL s_avrcp_play_status_valid = FALSE;

#define PCM_PAUSE_DIAG_INVALID_TIME ((DWORD)-1)

struct pcm_pause_diag_stats {
	BOOL active;
	DWORD start_ms;
	DWORD last_nonzero_ms;
	UINT32 packet_count;
	UINT32 byte_count;
	UINT32 nonzero_packet_count;
	UINT32 nonzero_byte_count;
	UINT32 peak_sample;
};

static struct pcm_pause_diag_stats s_pcm_pause_diag;

static void bt_porting_pcm_pause_diag_begin(void)
{
	if (s_pcm_pause_diag.active) {
		return;
	}

	memset(&s_pcm_pause_diag, 0, sizeof(s_pcm_pause_diag));
	s_pcm_pause_diag.active = TRUE;
	s_pcm_pause_diag.start_ms = GetCurrTime();
	s_pcm_pause_diag.last_nonzero_ms = PCM_PAUSE_DIAG_INVALID_TIME;
}

static void bt_porting_pcm_pause_diag_record(const unsigned char *data, unsigned int len)
{
	DWORD elapsed_ms;
	UINT32 i;
	UINT32 packet_peak = 0;
	BOOL packet_nonzero = FALSE;

	if (!s_pcm_pause_diag.active) {
		return;
	}

	elapsed_ms = GetCurrTime() - s_pcm_pause_diag.start_ms;
	s_pcm_pause_diag.packet_count++;
	s_pcm_pause_diag.byte_count += len;

	if (data == NULL || len == 0) {
		return;
	}

	for (i = 0; i < len; i++) {
		if (data[i] != 0) {
			s_pcm_pause_diag.nonzero_byte_count++;
			packet_nonzero = TRUE;
		}
	}

	for (i = 0; i + 1 < len; i += 2) {
		int sample = (int)(int16_t)((UINT16)data[i] | ((UINT16)data[i + 1] << 8));
		UINT32 magnitude = (UINT32)(sample < 0 ? -sample : sample);

		if (magnitude > packet_peak) {
			packet_peak = magnitude;
		}
	}

	if (packet_peak > s_pcm_pause_diag.peak_sample) {
		s_pcm_pause_diag.peak_sample = packet_peak;
	}

	if (packet_nonzero) {
		s_pcm_pause_diag.last_nonzero_ms = elapsed_ms;
		s_pcm_pause_diag.nonzero_packet_count++;
	}
}

static void bt_porting_pcm_pause_diag_finish(const char *reason)
{
	DWORD elapsed_ms;
	const char *result;
	int last_nonzero_ms;

	if (!s_pcm_pause_diag.active) {
		return;
	}

	elapsed_ms = GetCurrTime() - s_pcm_pause_diag.start_ms;
	if (s_pcm_pause_diag.packet_count == 0) {
		result = "NO_PCM";
	} else if (s_pcm_pause_diag.nonzero_byte_count == 0) {
		result = "ALL_ZERO";
	} else {
		result = "NONZERO_PCM";
	}

	last_nonzero_ms = s_pcm_pause_diag.last_nonzero_ms == PCM_PAUSE_DIAG_INVALID_TIME ?
		-1 : (int)s_pcm_pause_diag.last_nonzero_ms;

	LOG("[DEBUG-PCM-PAUSE] reason=%s result=%s elapsed_ms=%lu "
		"packets=%lu bytes=%lu nonzero_packets=%lu last_nonzero_ms=%d peak=%lu\n",
		reason, result, (unsigned long)elapsed_ms,
		(unsigned long)s_pcm_pause_diag.packet_count,
		(unsigned long)s_pcm_pause_diag.byte_count,
		(unsigned long)s_pcm_pause_diag.nonzero_packet_count,
		last_nonzero_ms, (unsigned long)s_pcm_pause_diag.peak_sample);

	s_pcm_pause_diag.active = FALSE;
}

static void bt_porting_audio_report_a2dp_state(BT_PLAY_STATE_E state, BOOL from_avrcp)
{
	if (!from_avrcp && s_avrcp_play_status_valid) {
		return;
	}

	if (state == s_last_a2dp_reported_state) {
		return;
	}

	if (s_play_state_cb != NULL) {
		LOG("[BT_PLAYSTATE] report source=%s old=%d new=%d sample=%d channel=2\n",
			from_avrcp ? "AVRCP" : "A2DP", s_last_a2dp_reported_state,
			state, s_a2dp_sample);
		s_play_state_cb(state, s_a2dp_sample, 2);
		s_last_a2dp_reported_state = state;
	}
}

void bt_porting_audio_report_avrcp_play_status(unsigned char play_status)
{
	BT_PLAY_STATE_E state;

	switch (play_status) {
	case AVRCP_LOGICAL_STATE_PLAYING:
		bt_porting_pcm_pause_diag_finish("AVRCP_PLAYING");
		state = BT_A2DP_PLAY_START;
		break;
	case AVRCP_LOGICAL_STATE_STOPPED:
		state = BT_A2DP_PLAY_STOP;
		break;
	case AVRCP_LOGICAL_STATE_PAUSED:
		bt_porting_pcm_pause_diag_begin();
		state = BT_A2DP_PLAY_STOP;
		break;
	default:
		LOG("[BT_PLAYSTATE] ignore unsupported AVRCP play_status=%u\n", play_status);
		return;
	}

	s_avrcp_play_status_valid = TRUE;
	bt_porting_audio_report_a2dp_state(state, TRUE);
}

void bt_porting_audio_reset_avrcp_play_status(void)
{
	bt_porting_pcm_pause_diag_finish("AVRCP_RESET");
	if (s_avrcp_play_status_valid) {
		LOG("[BT_PLAYSTATE] reset AVRCP authority last=%d\n", s_last_a2dp_reported_state);
	}
	s_avrcp_play_status_valid = FALSE;
}
#endif

void	bt_porting_register_play_state_callback_internal(bt_play_state_callback cb )
{
	s_play_state_cb = cb;
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
	s_last_a2dp_reported_state = BT_PLAY_STATE_END;
#endif
}

static bt_pcm_data_callback s_a2dp_data_cb = NULL;
void bt_porting_register_a2dp_data_callback_internal(bt_pcm_data_callback cb)
{
	s_a2dp_data_cb = cb;
}

static bt_pcm_data_callback s_hfp_spk_cb = NULL;
void bt_porting_register_hfp_spk_callback_internal(bt_pcm_data_callback cb)
{
	s_hfp_spk_cb = cb;
}

static void set_paused(int val)
{
#ifdef CHECK_EMPTY_DATA
    is_paused = val;
#endif
}

int send_sco_data(UINT16 handle, const void *data, unsigned int len)
{
    //LOG("play_sample len:%d\r\n",len);
#ifdef CONFIG_SCO_OVER_UART
    int slen = len;
    unsigned char *pdata = (unsigned char *) data;
    handle = s_sco_handle;

    if (handle == 0 || data == NULL || len == 0) {
        LOG("invalid param: handle:%x, len:%d, s_sco_pkt_len : %d.", handle, len, s_sco_pkt_len);
		return 0;
	}
	memset(s_sco_send_buf, 0, SCO_SEND_BUFFER_SIZE);
	s_sco_send_buf[0] = 3;
	s_sco_send_buf[1] = (unsigned char) handle & 0xFF;
	s_sco_send_buf[2] = (unsigned char) ((handle >> 8) & 0xFF);

//	LOG("handle:%x, len:%d, s_sco_pkt_len : %d.", handle, len, s_sco_pkt_len);
	s_sco_send_buf[3] = slen;
	memcpy(s_sco_send_buf + SCO_PKT_PREFIX_LEN, pdata, slen);
	/* FIXME Do NOT call TL function */
	hlp_tl_send_data(handle, s_sco_send_buf, slen + SCO_PKT_PREFIX_LEN);
#endif
	return 1;
}


static void bt_porting_audio_track_switch(int val)
{
	
}
static void bt_porting_audio_recorder_switch(int val)
{
	
}
void bt_porting_audio_switch(unsigned char type, int val)
{
	if (type == AUDIO_TYPE_A2DP) {
		bt_porting_audio_track_switch(val);
	} else { /* AUDIO_TYPE_SCO */
		bt_porting_audio_recorder_switch(val);
	}
}
void bt_porting_audio_open(unsigned char type, void *param)
{
	LOG("%s type = %d",__func__, type);
	if (type == AUDIO_TYPE_SCO) {
		audio_sco_param_t *sco_param = (audio_sco_param_t *) param;
		//HAL_CBACK(s_sco_callback, open_audio, sco_param->handle, sco_param->is_wbs);
		s_sco_handle = sco_param->handle;
		if (sco_param->is_wbs) {
			s_sco_sample = 16000;
		} else {
			s_sco_sample = 8000;
		}
		if(s_play_state_cb != NULL) {
			s_play_state_cb(BT_HFP_PLAY_START, s_sco_sample, 1);
		}
	} else {
		set_paused(0);
		//HAL_CBACK(s_a2dp_callback, open_audio, s_a2dp_sample);
		//s_play_state_cb(BT_A2DP_PLAY_START, s_a2dp_sample, 2); //cbk when start
	}
}

void bt_porting_audio_close(unsigned char type)
{
	LOG("bt_porting_audio_close -------------------------------- type = %d \n", type);

	if(type == AUDIO_TYPE_SCO) {
		//HAL_CBACK(s_sco_callback, close_audio);
		if(s_play_state_cb != NULL) {
			s_play_state_cb(BT_HFP_PLAY_STOP, s_sco_sample, 1);
		}
	} else {
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_pcm_pause_diag_finish("A2DP_CLOSE");
#endif
		set_paused(0);
		//HAL_CBACK(s_a2dp_callback, close_audio, AUDIO_TYPE_A2DP);
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_audio_reset_avrcp_play_status();
		bt_porting_audio_report_a2dp_state(BT_A2DP_PLAY_STOP, FALSE);
#else
		if(s_play_state_cb != NULL) {
			s_play_state_cb(BT_A2DP_PLAY_STOP, s_a2dp_sample, 2);
		}
#endif
	}
}

void bt_porting_audio_start(unsigned char type)
{
	if(type == AUDIO_TYPE_SCO) {
		//TODO
	} else {
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_pcm_pause_diag_finish("A2DP_PLAYING");
#endif
		set_paused(0);
		//HAL_CBACK(s_a2dp_callback, start_audio);
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_audio_report_a2dp_state(BT_A2DP_PLAY_START, FALSE);
#else
		if(s_play_state_cb != NULL) {
			s_play_state_cb(BT_A2DP_PLAY_START, s_a2dp_sample, 2);
		}
#endif
	}
}

void bt_porting_audio_pause(unsigned char type)
{
	if(type == AUDIO_TYPE_SCO) {
		//TODO
	} else {
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_pcm_pause_diag_finish("A2DP_SUSPEND");
#endif
		set_paused(1);
		//HAL_CBACK(s_a2dp_callback, stop_audio);
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
		bt_porting_audio_report_a2dp_state(BT_A2DP_PLAY_STOP, FALSE);
#else
		if(s_play_state_cb != NULL) {
			s_play_state_cb(BT_A2DP_PLAY_STOP, s_a2dp_sample, 2);
		}
#endif
	}
}

void bt_porting_audio_cfg(unsigned char type, unsigned char *param)
{
	s_a2dp_sample = (int) param;
}

static UINT8 bt_porting_audio_clamp_digital_volume(int volume)
{
	if (volume < 0) {
		return 0;
	} else if (volume > AUDIO_DIGITAL_VOLUME_MAX) {
		return AUDIO_DIGITAL_VOLUME_MAX;
	}

	return (UINT8)volume;
}

void bt_porting_audio_set_digital_volume(unsigned char type, unsigned char volume)
{
	UINT8 digital_volume = bt_porting_audio_clamp_digital_volume(volume);

	if (type == AUDIO_TYPE_A2DP) {
		s_a2dp_digital_volume = digital_volume;
	} else {
		s_hfp_digital_volume = digital_volume;
	}

	LOG("set digital volume type=%d volume=%d\n", type, digital_volume);
}

unsigned char bt_porting_audio_get_digital_volume(unsigned char type)
{
	if (type == AUDIO_TYPE_A2DP) {
		return s_a2dp_digital_volume;
	}

	return s_hfp_digital_volume;
}

void bt_porting_audio_set_gain(unsigned char type, float gain)
{
	int volume;

	if (gain <= 0.0f) {
		volume = 0;
	} else if (gain >= 1.0f) {
		volume = AUDIO_DIGITAL_VOLUME_MAX;
	} else {
		volume = (int)(gain * AUDIO_DIGITAL_VOLUME_MAX + 0.5f);
	}

	bt_porting_audio_set_digital_volume(type, (unsigned char)volume);
}

static void bt_porting_audio_apply_digital_volume(unsigned char *data, unsigned int len, UINT8 volume)
{
	unsigned int i;

	volume = bt_porting_audio_clamp_digital_volume(volume);
	if (data == NULL || len < 2 || volume >= AUDIO_DIGITAL_VOLUME_MAX) {
		return;
	}
	if (volume == 0) {
		memset(data, 0, len);
		return;
	}

	for (i = 0; i + 1 < len; i += 2) {
		int16_t sample = (int16_t)((UINT16)data[i] | ((UINT16)data[i + 1] << 8));
		int scaled = ((int)sample * volume) / AUDIO_DIGITAL_VOLUME_MAX;

		data[i] = (UINT8)(scaled & 0xFF);
		data[i + 1] = (UINT8)((scaled >> 8) & 0xFF);
	}
}

void bt_porting_audio_data_ind(unsigned char type, unsigned char *data, unsigned int len)
{
	if (type == AUDIO_TYPE_A2DP) {
		if (!is_paused) {
			//HAL_CBACK(s_a2dp_callback, write_audio, data, len);
			if(s_a2dp_data_cb != NULL) {
				bt_porting_audio_apply_digital_volume(data, len, s_a2dp_digital_volume);
#ifdef CONFIG_AVRCP_LOGICAL_PLAY_STATE_REPORT
				bt_porting_pcm_pause_diag_record(data, len);
#endif
				s_a2dp_data_cb(data, len);
			}
		}
	} else {
			//HAL_CBACK(s_sco_callback, write_audio, data, len);
			if(s_hfp_spk_cb != NULL) {
				bt_porting_audio_apply_digital_volume(data, len, s_hfp_digital_volume);
				s_hfp_spk_cb(data, len);
			}
	}
}

void app_register_audio_sink_callback(audio_a2dp_callbacks_t *cb)
{
	s_a2dp_callback = cb;
}

void app_register_audio_sco_callback(audio_sco_callbacks_t *cb)
{
	printf("app_register_audio_sco_callback------------------------------\n");
#ifdef CONFIG_AUDIO_DATA_TREATMENT_PLATFORM_SCO
	s_sco_callback = cb;
	if (cb) {
		HAL_CBACK(s_sco_callback, init, send_sco_data);
	} else {
		HAL_CBACK(s_sco_callback, done, send_sco_data);
	}
#endif
}

int app_get_micmute_stat()
{
	//printf("micmute_f = %d\n", micmute_f);
	return micmute_f;
}

void app_set_micmute_stat(int flag)
{
	printf("app_set_micmute_stat flag = %d\n", flag);
	micmute_f = flag;
}

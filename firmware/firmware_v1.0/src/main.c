#include <zephyr/kernel.h>
#include "transport.h"
#include "mic.h"
#include "utils.h"
#include "led.h"
#include "config.h"
#include "diagnostics.h"
#include "audio.h"
#include "codec.h"

static void codec_handler(uint8_t *data, size_t len)
{
	if (len == 0) { diagnostics_error(STREAM_CODEC, -EIO); return; }
	diagnostics_progress(STREAM_CODEC);
	broadcast_audio_packets(data, len); // Errors are logged inside
}

static void mic_handler(int16_t *buffer)
{
	diagnostics_progress(STREAM_MIC);
	int err = codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES);
	if (err) { diagnostics_error(STREAM_CODEC, err); }
}

void bt_ctlr_assert_handle(char *name, int type)
{
	diagnostics_error(STREAM_BLE, -EFAULT);
	if (name != NULL)
	{
		printk("Bt assert-> %s", name);
	}
}

bool is_connected = false;
bool is_charging = false;

void set_led_state()
{
	// Recording and connected state - BLUE
	if (is_connected && !diagnostics_has_fault())
	{
		set_led_red(false);
		set_led_green(false);
		set_led_blue(true);
		return;
	}

	// Recording but lost connection - RED
	if (!is_connected || diagnostics_has_fault())
	{
		set_led_red(true);
		set_led_green(false);
		set_led_blue(false);
		return;
	}

	// Not recording, but charging - WHITE
	if (is_charging)
	{
		set_led_red(true);
		set_led_green(true);
		set_led_blue(true);
		return;
	}

	// Not recording - OFF
	set_led_red(false);
	set_led_green(false);
	set_led_blue(false);
}

// Main loop
int main(void)
{
	uint32_t reason = NRF_POWER->RESETREAS;
	diagnostics_init(reason);
	NRF_POWER->RESETREAS = reason;
	printk("Boot reset reason=0x%08x\n", reason);
	// Led start
	ASSERT_OK(led_start());
	set_led_blue(true);

	// Transport start
	ASSERT_OK(transport_start());

	// Codec start
	set_codec_callback(codec_handler);
	ASSERT_OK(codec_start());

	// Mic start
	set_mic_callback(mic_handler);
	ASSERT_OK(mic_start());

	while (1)
	{
		diagnostics_poll(true);
		set_led_state();
		k_msleep(500);
	}

	// Unreachable
	return 0;
}

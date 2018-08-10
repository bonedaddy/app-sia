/*******************************************************************************
*   Ledger Blue
*   (c) 2016 Ledger
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include <stdint.h>
#include <stdbool.h>
#include "os.h"
#include "cx.h"
#include "os_io_seproxyhal.h"
#include "glyphs.h"
#include "sia.h"

// getPublicKey parameters
#define P2_DISPLAY_ADDRESS 0x00
#define P2_DISPLAY_PUBKEY  0x01

// exception codes
#define SW_INVALID_PARAM 0x6B00

unsigned char G_io_seproxyhal_spi_buffer[IO_SEPROXYHAL_BUFFER_SIZE_B];

typedef struct {
	uint32_t keyIndex;
	bool genAddr;
	uint8_t displayIndex;
	// NUL-terminated strings for display
	uint8_t typeStr[40]; // variable-length
	uint8_t keyStr[40]; // variable-length
	uint8_t fullStr[77]; // variable length
	uint8_t partialStr[13];
} getPublicKeyContext_t;

typedef struct signHashContext_t {
	uint32_t keyIndex;
	uint8_t hash[32];
	uint8_t hexHash[64];
	uint8_t displayIndex;
	// NUL-terminated strings for display
	uint8_t indexStr[40]; // variable-length
	uint8_t partialHashStr[13];
} signHashContext_t;

union {
	getPublicKeyContext_t getPublicKeyContext;
	signHashContext_t signHashContext;
} global;

// magic global variable implicitly referenced by the UX_ macros
ux_state_t ux;

// most screens have multiple "steps." ux_step selects which step is currently
// displayed; only bagl_elements whose userid field matches ux_step are
// displayed. ux_step_count is the total number of steps, so that we can cycle
// via ux_step = (ux_step+1) % ux_step_count.
unsigned int ux_step;
unsigned int ux_step_count;

const ux_menu_entry_t menu_main[];

const ux_menu_entry_t menu_about[] = {
	{NULL, NULL, 0, NULL, "Version", APPVERSION, 0, 0},
	{menu_main, NULL, 2, &C_icon_back, "Back", NULL, 61, 40},
	UX_MENU_END
};

const ux_menu_entry_t menu_main[] = {
	{NULL, NULL, 0, NULL, "Waiting for", "commands...", 0, 0},
	{menu_about, NULL, 0, NULL, "About", NULL, 0, 0},
	{NULL, os_sched_exit, 0, &C_icon_dashboard, "Quit app", NULL, 50, 29},
	UX_MENU_END
};

void ui_idle(void) {
	UX_MENU_DISPLAY(0, menu_main, NULL);
}

// helper macros for defining UI elements
#define UI_BACKGROUND() {{BAGL_RECTANGLE,0,0,0,128,32,0,0,BAGL_FILL,0,0xFFFFFF,0,0},NULL,0,0,0,NULL,NULL,NULL}
#define UI_ICON_LEFT(userid, glyph) {{BAGL_ICON,userid,3,12,7,7,0,0,0,0xFFFFFF,0,0,glyph},NULL,0,0,0,NULL,NULL,NULL}
#define UI_ICON_RIGHT(userid, glyph) {{BAGL_ICON,userid,117,13,8,6,0,0,0,0xFFFFFF,0,0,glyph},NULL,0,0,0,NULL,NULL,NULL}
#define UI_TEXT(userid, x, y, w, text) {{BAGL_LABELINE,userid,x,y,w,12,0,0,0,0xFFFFFF,0,BAGL_FONT_OPEN_SANS_REGULAR_11px|BAGL_FONT_ALIGNMENT_CENTER,0},(char *)text,0,0,0,NULL,NULL,NULL}

const bagl_element_t ui_getPublicKey_compare[] = {
	UI_BACKGROUND(),
	UI_ICON_LEFT(0x01, BAGL_GLYPH_ICON_LEFT),
	UI_ICON_RIGHT(0x02, BAGL_GLYPH_ICON_RIGHT),
	UI_TEXT(0x00, 0, 12, 128, global.getPublicKeyContext.typeStr),
	UI_TEXT(0x00, 0, 26, 128, global.getPublicKeyContext.partialStr),
};

const bagl_element_t* ui_prepro_getPublicKey_compare(const bagl_element_t *element) {
	getPublicKeyContext_t *ctx = &global.getPublicKeyContext;
	int fullSize = ctx->genAddr ? 76 : 44;

	// don't display arrows if we're at the end
	if ((element->component.userid == 1 && ctx->displayIndex == 0) ||
	    (element->component.userid == 2 && ctx->displayIndex == fullSize-12)) {
		return 0;
	}
	return element;
}

unsigned int ui_getPublicKey_compare_button(unsigned int button_mask, unsigned int button_mask_counter) {
	getPublicKeyContext_t *ctx = &global.getPublicKeyContext;
	int fullSize = ctx->genAddr ? 76 : 44;
	switch (button_mask) {
	case BUTTON_LEFT:
	case BUTTON_EVT_FAST | BUTTON_LEFT: // SEEK LEFT
		if (ctx->displayIndex > 0) {
			ctx->displayIndex--;
		}
		os_memmove(ctx->partialStr, ctx->fullStr+ctx->displayIndex, 12);
		UX_REDISPLAY();
		break;

	case BUTTON_RIGHT:
	case BUTTON_EVT_FAST | BUTTON_RIGHT: // SEEK RIGHT
		if (ctx->displayIndex < fullSize-12) {
			ctx->displayIndex++;
		}
		os_memmove(ctx->partialStr, ctx->fullStr+ctx->displayIndex, 12);
		UX_REDISPLAY();
		break;

	case BUTTON_EVT_RELEASED | BUTTON_LEFT | BUTTON_RIGHT: // PROCEED
		ui_idle();
		break;
	}
	return 0;
}

const bagl_element_t ui_getPublicKey_approve[] = {
	UI_BACKGROUND(),
	UI_ICON_LEFT(0x00, BAGL_GLYPH_ICON_CROSS),
	UI_ICON_RIGHT(0x00, BAGL_GLYPH_ICON_CHECK),
	UI_TEXT(0x00, 0, 12, 128, global.getPublicKeyContext.typeStr),
	UI_TEXT(0x00, 0, 26, 128, global.getPublicKeyContext.keyStr),
};

unsigned int ui_getPublicKey_approve_button(unsigned int button_mask, unsigned int button_mask_counter) {
	getPublicKeyContext_t *ctx = &global.getPublicKeyContext;
	uint16_t tx = 0;
	cx_ecfp_public_key_t publicKey;
	switch (button_mask) {
	case BUTTON_EVT_RELEASED | BUTTON_LEFT: // REJECT
		G_io_apdu_buffer[tx++] = 0x69;
		G_io_apdu_buffer[tx++] = 0x85;
		io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
		ui_idle();
		break;

	case BUTTON_EVT_RELEASED | BUTTON_RIGHT: // APPROVE
		// derive pubkey and address
		deriveSiaKeypair(ctx->keyIndex, NULL, &publicKey);
		extractPubkeyBytes(G_io_apdu_buffer + tx, &publicKey);
		tx += 32;
		pubkeyToSiaAddress(G_io_apdu_buffer + tx, &publicKey);
		tx += 76;

		// prepare comparison screen
		if (ctx->genAddr) {
			os_memmove(ctx->typeStr, "Compare:", 9);
			os_memmove(ctx->fullStr, G_io_apdu_buffer + 32, 76);
			ctx->fullStr[76] = '\0';
		} else {
			os_memmove(ctx->typeStr, "Compare:", 9);
			bin2b64(ctx->fullStr, G_io_apdu_buffer, 32);
		}
		os_memmove(ctx->partialStr, ctx->fullStr, 12);
		ctx->partialStr[12] = '\0';

		// send response
		G_io_apdu_buffer[tx++] = 0x90;
		G_io_apdu_buffer[tx++] = 0x00;
		io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);

		// display comparison screen
		ctx->displayIndex = 0;
		UX_DISPLAY(ui_getPublicKey_compare, ui_prepro_getPublicKey_compare);
		break;
	}
	return 0;
}

// handleGetPublicKey reads a key index, derives the corresponding public key,
// converts it to a Sia address, stores the address in the global context, and
// displays the getPublicKey UI.
void handleGetPublicKey(uint8_t p1, uint8_t p2, uint8_t *dataBuffer, uint16_t dataLength, volatile unsigned int *flags, volatile unsigned int *tx) {
	if ((p2 != P2_DISPLAY_ADDRESS) && (p2 != P2_DISPLAY_PUBKEY)) {
		THROW(SW_INVALID_PARAM);
	}
	getPublicKeyContext_t *ctx = &global.getPublicKeyContext;

	// read key index and genAddr flag
	ctx->keyIndex = U4LE(dataBuffer, 0);
	ctx->genAddr = (p2 == P2_DISPLAY_ADDRESS);

	if (p2 == P2_DISPLAY_ADDRESS) {
		os_memmove(ctx->typeStr, "Generate Address", 17);
		os_memmove(ctx->keyStr, "from Key #", 10);
		int n = bin2dec(ctx->keyStr+10, ctx->keyIndex);
		os_memmove(ctx->keyStr+10+n, "?", 2);
	} else if (p2 == P2_DISPLAY_PUBKEY) {
		os_memmove(ctx->typeStr, "Generate Public", 16);
		os_memmove(ctx->keyStr, "Key #", 5);
		int n = bin2dec(ctx->keyStr+5, ctx->keyIndex);
		os_memmove(ctx->keyStr+5+n, "?", 2);
	}

	// display approval screen
	ux_step = 0;
	ux_step_count = 1;
	UX_DISPLAY(ui_getPublicKey_approve, NULL);

	*flags |= IO_ASYNCH_REPLY;
}

const bagl_element_t ui_signHash_approve[] = {
	UI_BACKGROUND(),
	UI_ICON_LEFT(0x00, BAGL_GLYPH_ICON_CROSS),
	UI_ICON_RIGHT(0x00, BAGL_GLYPH_ICON_CHECK),
	UI_TEXT(0x00, 0, 12, 128, "Sign this Hash"),
	UI_TEXT(0x00, 0, 26, 128, global.signHashContext.indexStr),
};

unsigned int ui_signHash_approve_button(unsigned int button_mask, unsigned int button_mask_counter) {
	signHashContext_t *ctx = &global.signHashContext;
	uint16_t tx = 0;
	switch (button_mask) {
	case BUTTON_EVT_RELEASED | BUTTON_LEFT: // REJECT
		G_io_apdu_buffer[tx++] = 0x69;
		G_io_apdu_buffer[tx++] = 0x85;
		io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
		ui_idle();
		break;

	case BUTTON_EVT_RELEASED | BUTTON_RIGHT: // APPROVE
		deriveAndSign(ctx->keyIndex, ctx->hash, G_io_apdu_buffer);
		tx += 64;
		G_io_apdu_buffer[tx++] = 0x90;
		G_io_apdu_buffer[tx++] = 0x00;
		io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, tx);
		ui_idle();
		break;
	}
	return 0;
}

const bagl_element_t ui_signHash_compare[] = {
	UI_BACKGROUND(),
	UI_ICON_LEFT(0x01, BAGL_GLYPH_ICON_LEFT),
	UI_ICON_RIGHT(0x02, BAGL_GLYPH_ICON_RIGHT),
	UI_TEXT(0x00, 0, 12, 128, "Compare Hashes:"),
	UI_TEXT(0x00, 0, 26, 128, global.signHashContext.partialHashStr),
};

const bagl_element_t* ui_prepro_signHash_compare(const bagl_element_t *element) {
	signHashContext_t *ctx = &global.signHashContext;

	// don't display arrows if we're at the end
	if ((element->component.userid == 1 && ctx->displayIndex == 0) ||
	    (element->component.userid == 2 && ctx->displayIndex == sizeof(ctx->hexHash)-12)) {
		return 0;
	}
	return element;
}

unsigned int ui_signHash_compare_button(unsigned int button_mask, unsigned int button_mask_counter) {
	signHashContext_t *ctx = &global.signHashContext;

	switch (button_mask) {
	case BUTTON_LEFT:
	case BUTTON_EVT_FAST | BUTTON_LEFT: // SEEK LEFT
		if (ctx->displayIndex > 0) {
			ctx->displayIndex--;
		}
		os_memmove(ctx->partialHashStr, ctx->hexHash+ctx->displayIndex, 12);
		UX_REDISPLAY();
		break;

	case BUTTON_RIGHT:
	case BUTTON_EVT_FAST | BUTTON_RIGHT: // SEEK RIGHT
		if (ctx->displayIndex < sizeof(ctx->hexHash)-12) {
			ctx->displayIndex++;
		}
		os_memmove(ctx->partialHashStr, ctx->hexHash+ctx->displayIndex, 12);
		UX_REDISPLAY();
		break;

	case BUTTON_EVT_RELEASED | BUTTON_LEFT | BUTTON_RIGHT: // PROCEED
		// display approval screen
		UX_DISPLAY(ui_signHash_approve, NULL);
		break;
	}
	return 0;
}

// handleSignHash reads a key index and a hash, stores them in the global
// context, and displays the signHash UI.
void handleSignHash(uint8_t p1, uint8_t p2, uint8_t *dataBuffer, uint16_t dataLength, volatile unsigned int *flags, volatile unsigned int *tx) {
	signHashContext_t *ctx = &global.signHashContext;

	// read key index
	ctx->keyIndex = U4LE(dataBuffer, 0);
	os_memmove(ctx->indexStr, "with Key #", 10);
	int n = bin2dec(ctx->indexStr+10, ctx->keyIndex);
	os_memmove(ctx->indexStr+10+n, "?", 2);

	// read hash to sign
	os_memmove(ctx->hash, dataBuffer+4, 32);

	// convert hash to hex and display the first 12 hex digits
	bin2hex(ctx->hexHash, ctx->hash, 32);
	os_memmove(ctx->partialHashStr, ctx->hexHash, 12);
	ctx->partialHashStr[12] = '\0';
	ctx->displayIndex = 0;

	// display comparison screen
	ux_step = 0;
	ux_step_count = 1;
	UX_DISPLAY(ui_signHash_compare, ui_prepro_signHash_compare);

	*flags |= IO_ASYNCH_REPLY;
}

// The APDU protocol uses a single-byte instruction code (INS) to specify
// which command should be executed. We use this code to dispatch on a table
// of function pointers.

#define INS_GET_PUBLIC_KEY 0x01
#define INS_SIGN_HASH      0x02

typedef void handler_fn_t(uint8_t, uint8_t, uint8_t *, uint16_t , volatile unsigned int *, volatile unsigned int *);

handler_fn_t* lookupHandler(uint8_t ins) {
	switch (ins) {
	case INS_GET_PUBLIC_KEY: return handleGetPublicKey;
	case INS_SIGN_HASH:      return handleSignHash;
	}
	return NULL;
}

// Everything below this point is Ledger magic. Don't bother trying to
// understand it.

unsigned short io_exchange_al(unsigned char channel, unsigned short tx_len) {
	switch (channel & ~(IO_FLAGS)) {
	case CHANNEL_KEYBOARD:
		break;
	// multiplexed io exchange over a SPI channel and TLV encapsulated protocol
	case CHANNEL_SPI:
		if (tx_len) {
			io_seproxyhal_spi_send(G_io_apdu_buffer, tx_len);
			if (channel & IO_RESET_AFTER_REPLIED) {
				reset();
			}
			return 0; // nothing received from the master so far (it's a tx transaction)
		} else {
			return io_seproxyhal_spi_recv(G_io_apdu_buffer, sizeof(G_io_apdu_buffer), 0);
		}
	default:
		THROW(INVALID_PARAMETER);
	}
	return 0;
}

#define CLA          0xE0
#define OFFSET_CLA   0x00
#define OFFSET_INS   0x01
#define OFFSET_P1    0x02
#define OFFSET_P2    0x03
#define OFFSET_LC    0x04
#define OFFSET_CDATA 0x05

void sia_main(void) {
	volatile unsigned int rx = 0;
	volatile unsigned int tx = 0;
	volatile unsigned int flags = 0;

	for (;;) {
		volatile unsigned short sw = 0;

		BEGIN_TRY {
			TRY {
				rx = tx;
				tx = 0; // ensure no race in catch_other if io_exchange throws an error
				rx = io_exchange(CHANNEL_APDU | flags, rx);
				flags = 0;

				// no apdu received; reset the session and reset the bootloader configuration
				if (rx == 0) {
					THROW(0x6982);
				}
				if (G_io_apdu_buffer[OFFSET_CLA] != CLA) {
					THROW(0x6E00);
				}
				// call handler function associated with this instruction
				handler_fn_t *handlerFn = lookupHandler(G_io_apdu_buffer[OFFSET_INS]);
				if (!handlerFn) {
					THROW(0x6D00);
				}
				handlerFn(G_io_apdu_buffer[OFFSET_P1], G_io_apdu_buffer[OFFSET_P2], G_io_apdu_buffer + OFFSET_CDATA, G_io_apdu_buffer[OFFSET_LC], &flags, &tx);
			}
			CATCH(EXCEPTION_IO_RESET) {
				THROW(EXCEPTION_IO_RESET);
			}
			CATCH_OTHER(e) {
				switch (e & 0xF000) {
				case 0x6000:
					// report the exception
					sw = e;
					break;
				case 0x9000:
					// All is well
					sw = e;
					break;
				default:
					// Internal error
					sw = 0x6800 | (e & 0x7FF);
					break;
				}
				// Unexpected exception => report
				G_io_apdu_buffer[tx] = sw >> 8;
				G_io_apdu_buffer[tx + 1] = sw;
				tx += 2;
			}
			FINALLY {
			}
		}
		END_TRY;
	}
}

// override point, but nothing more to do
void io_seproxyhal_display(const bagl_element_t *element) {
	io_seproxyhal_display_default((bagl_element_t *)element);
}

unsigned char io_event(unsigned char channel) {
	// can't have more than one tag in the reply, not supported yet.
	switch (G_io_seproxyhal_spi_buffer[0]) {
	case SEPROXYHAL_TAG_FINGER_EVENT:
		UX_FINGER_EVENT(G_io_seproxyhal_spi_buffer);
		break;

	case SEPROXYHAL_TAG_BUTTON_PUSH_EVENT:
		UX_BUTTON_PUSH_EVENT(G_io_seproxyhal_spi_buffer);
		break;

	case SEPROXYHAL_TAG_STATUS_EVENT:
		if (G_io_apdu_media == IO_APDU_MEDIA_USB_HID &&
			!(U4BE(G_io_seproxyhal_spi_buffer, 3) &
			  SEPROXYHAL_TAG_STATUS_EVENT_FLAG_USB_POWERED)) {
			THROW(EXCEPTION_IO_RESET);
		}
		UX_DEFAULT_EVENT();
		break;

	case SEPROXYHAL_TAG_DISPLAY_PROCESSED_EVENT:
		UX_DISPLAYED_EVENT({});
		break;

	case SEPROXYHAL_TAG_TICKER_EVENT:
		UX_TICKER_EVENT(G_io_seproxyhal_spi_buffer, {
			if (UX_ALLOWED) {
				if (ux_step_count) {
					// prepare next screen
					ux_step = (ux_step + 1) % ux_step_count;
					// redisplay screen
					UX_REDISPLAY();
				}
			}
		});
		break;

	default:
		UX_DEFAULT_EVENT();
		break;
	}

	// close the event if not done previously (by a display or whatever)
	if (!io_seproxyhal_spi_is_status_sent()) {
		io_seproxyhal_general_status();
	}

	// command has been processed, DO NOT reset the current APDU transport
	return 1;
}

void app_exit(void) {
	BEGIN_TRY_L(exit) {
		TRY_L(exit) {
			os_sched_exit(-1);
		}
		FINALLY_L(exit) {
		}
	}
	END_TRY_L(exit);
}

__attribute__((section(".boot"))) int main(void) {
	// exit critical section
	__asm volatile("cpsie i");

	for (;;) {
		UX_INIT();

		// ensure exception will work as planned
		os_boot();

		BEGIN_TRY {
			TRY {
				io_seproxyhal_init();

				USB_power(0);
				USB_power(1);

				ui_idle();

				sia_main();
			}
			CATCH(EXCEPTION_IO_RESET) {
				// reset IO and UX before continuing
				continue;
			}
			CATCH_ALL {
				break;
			}
			FINALLY {
			}
		}
		END_TRY;
	}
	app_exit();

	return 0;
}

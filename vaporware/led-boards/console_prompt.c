#include "console_prompt.h"

#include <stdio.h>

#include "color.h"
#include "config.h"
#include "console.h"
#include "error.h"
#include "fixedpoint.h"
#include "git_version.h"
#include "pwm.h"
#include "term.h"

#include "stm_include/stm32/scb.h"

/*
 * Function type for config console commands.  The function receives
 * the parameters given on the console already parsed as integers.
 */
typedef error_t (*command_handler_t)(unsigned int[]);

/*
 * This represents one command possible on the console.
 */
typedef struct {
	char key; // The key with which the command is invoked.
	int arg_length; // The number of arguments expected.
	command_handler_t handler; // The handler function for this command.
	char *usage; // A message to display when the command has not
		     // been successful or as a help text.
	int does_exit; // The config console should exit after this
		       // command has been successfully run.
} console_command_t;

#define LINE_LENGTH 80

static const char *ADDR_OUT_OF_RANGE =
	"Address out of range (0x00 to 0xfd)" CRLF;

static const char *WARN_BROADCAST_ADDR =
	"Warning: Setting address to broadcast" CRLF;

static const char *CHANNEL_OUT_OF_RANGE =
	"PWM channel index out of range (0 to " XSTR(MODULE_LENGTH) "-1)" CRLF;

static const char *LED_OUT_OF_RANGE =
	"RGB LED index out of range (0 to " XSTR(RGB_LED_COUNT) "-1)" CRLF;

static const char *BRIGHTNESS_OUT_OF_RANGE =
	"Brightness out of range (0 to 0xffff)" CRLF;

static const char *SENSOR_OUT_OF_RANGE =
	"Heat sensor index out of range (0 to "
	XSTR(HEAT_SENSOR_LEN) "-1)" CRLF;

static const char *HEAT_LIMIT_OUT_OF_RANGE =
	"Heat limit out of range (0 to 0xffff)" CRLF;

static const char *NO_CONFIG_FOUND =
	"No configuration in flash" CRLF;

static const char *UNKNOWN_FLASH_ERROR =
	"Internal flash error" CRLF;

static const char *FLASH_WRITE_FAILED =
	"Writing to flash failed." CRLF;

static const char *CONFIG_IS_INVALID =
	"Invalid configuration state." CRLF;

static const char *RELOADING_CONFIG =
	"Reloading configuration..." CRLF;

static const char *SAVING_CONFIG =
	"Saving configuration..." CRLF;

static const char *BEGINNING_ECHO =
	"Echoing... Finish with q on a single line" CRLF;

static const char *PASTE_NOW =
	"Paste a file with one command per line, finish with q" CRLF;

static const char *ENTER_MATRIX =
	"Enter correction matrix" CRLF;

static const char *ENTER_MAX_Y =
	"Enter maximum Y value" CRLF;

/*
 * Checks that the given value is greater than or equal to 0 and less
 * then the given limit. Prints the given message and returns
 * E_ARG_FORMAT if this is not the case. Returns E_SUCCESS otherwise.
 */
static error_t check_range(int value, int limit, const char *message) {
	if (value < 0 || value >= limit) {
		console_write(message);
		return E_ARG_FORMAT;
	} else {
		return E_SUCCESS;
	}
}

/*
 * Checks that the given index is a valid PWM channel index.
 */
static error_t check_channel_index(int index, const char *message) {
	return check_range(index, MODULE_LENGTH, message);
}

/*
 * Checks that the given index is a valid PWM channel index.
 */
static error_t check_led_index(int index, const char *message) {
	return check_range(index, RGB_LED_COUNT, message);
}

/*
 * Checks that the given index is an unsigned 16-bit value.
 */
static error_t check_short(int value, const char *message) {
	return check_range(value, 0x10000, message);
}

/*
 * Runs the "set module address" command.
 *
 * Expected format for args: { address }
 *
 * Returns E_ARG_FORMAT if the given address is out of range.
 */
static error_t run_set_addr(unsigned int args[]) {
	int addr = args[0];

	// The allowable range for an address is 0x00 to 0xfe
	// (0xff is reserved)
	if (addr < 0 || addr > 0xfd) {
		console_write(ADDR_OUT_OF_RANGE);
		return E_ARG_FORMAT;
	}
	if (addr == 0xfd) {
		console_write(WARN_BROADCAST_ADDR);
	}

	config.my_address = addr;
	return E_SUCCESS;
}

/*
 * Runs the "set brightness" command.
 *
 * Expected format for args: { channel-index, brightness }
 *
 * Returns E_ARG_FORMAT if the PWM channel index or brightness is out
 * of range.  Also passes on the errors from pwm_set_brightness.
 */
static error_t run_set_brightness(unsigned int args[]) {
	int index = args[0];
	int brightness = args[1];

	if (check_channel_index(index, CHANNEL_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}
	if (check_short(brightness, BRIGHTNESS_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	uint8_t pwm_channel = convert_channel_index(index);
	error_t error = pwm_set_brightness(pwm_channel, (uint16_t) brightness);
	if (error) return error;
	return pwm_send_frame();
}

/*
 * Runs the "set LED color" command.
 *
 * Expected format for args: { led-index, x, y, Y }
 *
 * Returns E_ARG_FORMAT if the RGB LED index is out of range. Also
 * passes on the errors from pwm_set_brightness.
 */
static error_t run_set_color(unsigned int args[]) {
	int index = args[0];
	int x = args[1];
	int y = args[2];
	int Y = args[3];

	if (check_led_index(index, LED_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	led_info_t info = config.led_infos[index];

	uint16_t rgb[3];
	color_correct(info, x, y, Y, rgb);

	console_write("Color correction: ");
	console_uint_d(rgb[RED]); console_write(" ");
	console_uint_d(rgb[GREEN]); console_write(" ");
	console_uint_d(rgb[BLUE]); console_write(CRLF);

	for(int i = 0; i < 3; i++) {
		error_t error = pwm_set_brightness(info.channels[i], rgb[i]);
		if (error) return error;
	}

	return pwm_send_frame();
}

static const char *COLOR_NAMES[] = {
	"red" CRLF,
	"green" CRLF,
	"blue" CRLF,
};

static const char *VALUE_NAMES[] = {
	"x (in 65536ths) = ",
	"y (in 65536ths) = ",
	"Y (integer part) = ",
	"Y (fractional part in 65536ths) = "
};

/*
 * Runs the "calibrate LED" command.
 *
 * Expected format for args: { led-index }
 *
 * Returns E_ARG_FORMAT if the RGB LED index is out of range.
 */
static error_t run_calibrate_led(unsigned int args[]) {
	int index = args[0];

	if (check_led_index(index, LED_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	fixed_t matrix[9];

	for (int c = 0; c < 3; c++) {
		console_write(COLOR_NAMES[c]);
		for (int v = 0; v < 2; v++) {
			unsigned int input = console_ask_int(VALUE_NAMES[v], 10);
			matrix[3 * v + c] = fixfract(input);
		}

		unsigned int Y_int = console_ask_int(VALUE_NAMES[2], 10);
		unsigned int Y_frac = console_ask_int(VALUE_NAMES[3], 10);

		fixed_t Y = fixadd(fixnum(Y_int), fixfract(Y_frac));
		config.led_infos[index].peak_Y[c] = Y;
	}

	// Fill in the homogenous coordinate entries
	for (int i = 0; i < 3; i++) {
		matrix[6 + i] = FIXNUM(1.0);
	}

	fixed_t *out = config.led_infos[index].color_matrix;
	invert_3x3(matrix, out);

	return E_SUCCESS;
}

/*
 * Runs the "enter echo mode" command.
 *
 * Expected format for args: { }
 *
 * Always succeeds.
 */
static error_t run_echo(unsigned int args[]) {
	(void)args;
	char buf[80];

	console_write(BEGINNING_ECHO);
	do {
		console_getline(buf, 80);
	} while (buf[0] == 'q' && buf[1] == '\0');

	return E_SUCCESS;
}

/*
 * Runs the "paste command file" command.
 *
 * Expected format for args: { }
 *
 * Always succeeds (although the commands in the file may not).
 */
static error_t run_paste_file(unsigned int args[]) {
	(void)args;
	console_write(PASTE_NOW);

	int should_exit = 0;

	do {
		should_exit = run_command_prompt();
	} while(!should_exit);

	return E_SUCCESS;
}

/*
 * Runs the "set heat limit" command.
 *
 * Expected format for args: { sensor-index, heat-limit }
 *
 * Returns E_ARG_FORMAT if the heat sensor index or the limit is out of range.
 */
static error_t run_set_heat_limit(unsigned int args[]) {
	int index = args[0];
	int limit = args[1];

	if (check_range(index, HEAT_SENSOR_LEN, SENSOR_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}
	if (check_short(limit, HEAT_LIMIT_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	config.heat_limit[index] = limit;
	return E_SUCCESS;
}

/*
 * Runs the "reload configuration from flash" command.
 *
 * Expected format for args: {  }
 *
 * Returns the error reported by load_config.
 */
static error_t run_reload_config(unsigned int args[]) {
	(void)args;
	console_write(RELOADING_CONFIG);

	error_t error = load_config();

	switch (error) {
	case E_SUCCESS:
		break;
	case E_NOCONFIG:
		console_write(NO_CONFIG_FOUND);
		break;
	default:
		console_write(UNKNOWN_FLASH_ERROR);
		break;
	}

	return error;
}

/*
 * Runs the "set correction matrix" command.
 *
 * Expected format for args: { led-index }
 *
 * Returns E_ARG_FORMAT if the LED index is out of range.
 */
static error_t run_set_correction(unsigned int args[]) {
	int led = args[0];

	if (check_led_index(led, LED_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	console_write(ENTER_MATRIX);

	for (int i = 0; i < 9; i++) {
		int input = console_ask_int("", 10);
		config.led_infos[led].color_matrix[i] = (fixed_t){ input };
	}

	return E_SUCCESS;
}

/*
 * Runs the "set PWM channels" command.
 *
 * Expected format for args: { led-index, channel-r, channel-g, channel-b }
 *
 * Returns the error reported by load_config.
 */
static error_t run_set_pwm_channels(unsigned int args[]) {
	int led = args[0];
	int rgb[3] = {
		args[1],
		args[2],
		args[3]
	};

	if (check_led_index(led, LED_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}
	for (int i = 0; i < 3; i++) {
		if (check_channel_index(rgb[i], CHANNEL_OUT_OF_RANGE)) {
			return E_ARG_FORMAT;
		}
	}

	for(int i = 0; i < 3; i++) {
		config.led_infos[led].channels[i] = rgb[i];
	}

	return E_SUCCESS;
}

/*
 * Runs the "quit" command.
 *
 * This function always succeeds and returns E_SUCCESS.
 */
static error_t run_quit(unsigned int args[]) {
	(void)args;
	return E_SUCCESS;
}

/*
 * Runs the "reset module" command.
 *
 * Expected format for args: { }
 *
 * This function does not return.
 */
static error_t run_reset(unsigned int args[]) {
	(void)args;
	SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESETREQ;
	return E_SUCCESS;
}

/*
 * Runs the "save configuration to flash" command.
 *
 * Expected format for args: { }
 *
 * Returns the error reported by save_config.
 */
static error_t run_save_config(unsigned int args[]) {
	(void)args;
	if (!config_valid(config)) {
		console_write(CONFIG_IS_INVALID);
		return E_NOCONFIG;
	}

	console_write(SAVING_CONFIG);

	error_t error = save_config();

	switch(error) {
	case E_SUCCESS:
		break;
	case E_FLASH_WRITE:
		console_write(FLASH_WRITE_FAILED);
		break;
	default:
		console_write(UNKNOWN_FLASH_ERROR);
		break;
	}

	return error;
}

/*
 * Runs the "set maximum Y value" command.
 *
 * Expected format for args: { led-index }
 *
 * Returns E_ARG_FORMAT if the LED index is out of range.
 */
static error_t run_set_max_Y(unsigned int args[]) {
	int led = args[0];

	if (check_led_index(led, LED_OUT_OF_RANGE)) {
		return E_ARG_FORMAT;
	}

	console_write(ENTER_MAX_Y);

	for (int i = 0; i < 3; i++) {
		int input = console_ask_int("", 16);
		config.led_infos[led].peak_Y[i] = (fixed_t){ input };
	}

	return E_SUCCESS;
}

/*
 * This is actually implemented after the command array, because it
 * needs access to it.
 */
static error_t run_help(unsigned int args[]);

static console_command_t commands[] = {
	{
		.key = 'a',
		.arg_length = 1,
		.handler = run_set_addr,
		.usage = "a <address>: Set module address",
		.does_exit = 0,
	},
	{
		.key = 'b',
		.arg_length = 2,
		.handler = run_set_brightness,
		.usage =
		"b <channel> <brightness>: Set brightness of a single PWM channel",
		.does_exit = 0,
	},
	{
		.key = 'c',
		.arg_length = 4,
		.handler = run_set_color,
		.usage = "c <led> <x> <y> <Y>: Switch LED to xyY color",
		.does_exit = 0,
	},
	{
		.key = 'C',
		.arg_length = 1,
		.handler = run_calibrate_led,
		.usage = "C <led>: Set calibration of LED",
		.does_exit = 0,
	},
	{
		.key = 'e',
		.arg_length = 0,
		.handler = run_echo,
		.usage = "e: Begin echo mode",
		.does_exit = 0,
	},
	{
		.key = 'f',
		.arg_length = 0,
		.handler = run_paste_file,
		.usage = "f: Paste a command file",
		.does_exit = 0,
	},
	{
		.key = 'h',
		.arg_length = 2,
		.handler = run_set_heat_limit,
		.usage = "h <sensor> <heat-limit>: Set heat limit",
		.does_exit = 0,
	},
	{
		.key = 'l',
		.arg_length = 0,
		.handler = run_reload_config,
		.usage = "l: Reload configuration",
		.does_exit = 0,
	},
	{
		.key = 'm',
		.arg_length = 1,
		.handler = run_set_correction,
		.usage = "m <led>: set an LED's correction matrix",
		.does_exit = 0,
	},
	{
		.key = 'p',
		.arg_length = 4,
		.handler = run_set_pwm_channels,
		.usage = "p <led> <r-chan> <g-chan> <b-chan>: set an LED's PWM channels",
		.does_exit = 0,
	},
	{
		.key = 'q',
		.arg_length = 0,
		.handler = run_quit,
		.usage = "q: Quit to normal mode",
		.does_exit = 1,
	},
	{
		.key = 'r',
		.arg_length = 0,
		.handler = run_reset,
		.usage = "r: Reset",
		.does_exit = 0,
	},
	{
		.key = 's',
		.arg_length = 0,
		.handler = run_save_config,
		.usage = "s: Save configuration",
		.does_exit = 0,
	},
	{
		.key = 'y',
		.arg_length = 1,
		.handler = run_set_max_Y,
		.usage = "y <led-index>: Set maximum Y value for LED",
		.does_exit = 0,
	},
	{
		.key = '?',
		.arg_length = 0,
		.handler = run_help,
		.usage = "?: Show command usage messages",
		.does_exit = 0,
	},
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(console_command_t))
#define MAX_ARG_LEN 4

/*
 * Runs the "show help" command.
 *
 * Expected format for args: { }
 *
 * This function always succeeds and returns E_SUCCESS;
 */
static error_t run_help(unsigned int args[]) {
	(void)args;
	for (unsigned i = 0; i < COMMAND_COUNT; i++) {
		console_write(commands[i].usage);
		console_write(CRLF);
	}

	console_write(CRLF);

	return E_SUCCESS;
}

static const char *PROGRAM_ID =
	"vaporware build " GIT_VERSION_ID CRLF;

static const char *MODULE_ADDRESS =
	"Module address: ";

static const char *IS_BROADCAST =
	" (broadcast)";

static const char *HEAT_SETTINGS_HEAD =
	"Heat sensor settings:" CRLF
        "Sensor  Limit" CRLF;

static const char *LED_SETTINGS_HEAD =
	"LED settings:" CRLF
	"LED  channel  correction matrix            Y_max" CRLF;

static const char *CONSOLE_PROMPT =
	"> ";

/*
 * Displays the current configuration and a prompt on the debug
 * console.
 */
void show_status_prompt() {

	// Sketch for the config console screen
/*
vaporlight build 0000000000000000000000000000000000000000
This is module 99

Heat sensor settings:
Sensor   Limit
    99   9999
    99   9999
    99   9999
    99   9999
    99   9999
    99   9999

LED settings:
LED  channel  correction matrix           Y_max
 99  99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
 99  99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
 99  99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
 99  99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
 99  99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
     99       ffffffff ffffffff ffffffff  ffffffff
>
*/


	console_write(PROGRAM_ID);

	console_write(MODULE_ADDRESS);
	console_uint_3d(config.my_address);
	if (config.my_address == 0xfd) {
		console_write(IS_BROADCAST);
	}
	console_write(CRLF CRLF);

	console_write(HEAT_SETTINGS_HEAD);

	for (int i = 0; i < HEAT_SENSOR_LEN; i++) {
		console_write("   ");
		console_uint_2d(i);
		console_write("   ");
		console_uint_5d(config.heat_limit[i]);
		console_write(CRLF);
	}
	console_write(CRLF);

	console_write(LED_SETTINGS_HEAD);
	for (int l = 0; l < RGB_LED_COUNT; l++) {
		led_info_t info = config.led_infos[l];

		for (int c = 0; c < 3; c++) {
			if (c == 0) {
				console_uint_3d(l);
			} else {
				console_write("   ");
			}
			console_write("  ");

			console_uint_2d(info.channels[c]);
			console_write("       ");

			for (int i = 0; i < 3; i++) {
				console_fixed(info.color_matrix[3*c+i], 10);
				console_putchar(' ');
			}
			console_write("  ");

			console_fixed(info.peak_Y[c], 10);
			console_write(CRLF);
		}
		console_write(CRLF);
	}

	console_write(CONSOLE_PROMPT);
}

/*
 * Looks for the command with the given key in the commands table
 * provided by the console_commands module and returns a pointer to
 * its console_command_t instance if found. If no command has been
 * found, returns NULL.
 */
static console_command_t *get_command(char key) {
	for (unsigned i = 0; i < COMMAND_COUNT; i++) {
		if (commands[i].key == key) {
			return &commands[i];
		}
	}

	return NULL;
}

#define SKIP_WHILE(pred, string, position)  \
	while(pred((int)((string)[(position)]))) {	\
		(position)++;		    \
	}

static int isalnum(int x) {
	char c = (char) x;
	return ('a' <= c && c <= 'z') ||
		('0' <= c && c <= '9') ||
		('A' <= c && c <= 'Z');
}

static int isspace(int x) {
	char c = (char) x;
	return c == ' ' || c == '\n' || c == '\t' || c == '\f' || c == '\v' || c == '\r';
}

/*
 * Parses a command line of the format "<command-key> <integer-argument>*".
 *
 * arg_length specifies how may arguments are expected. If there are
 * more arguments present, they are ignored; if there are less,
 * E_MISSING_ARGS is returned. The arguments are converted to integers
 * and stored in args[0]...args[arg_length-1]. If any of the
 * conversions fails, E_ARGUMENT_FORMAT is returned.
 */
static error_t parse_args(char *line, unsigned int *args, int arg_length) {
	int pos = 0;

	// Skip over the command-key and following space.
	SKIP_WHILE(isalnum, line, pos);
	SKIP_WHILE(isspace, line, pos);

	for (int arg = 0; arg < arg_length; arg++) {
		if (line[pos] == '\0') {
			// The line has ended before all args could be parsed.
			return E_MISSING_ARGS;
		}

		_Static_assert(CONSOLE_READ_BASE <= MAX_BASE, "CONSOLE_READ_BASE out of range");
		error_t error = parse_int(line, &pos, &args[arg], CONSOLE_READ_BASE);

		if (error) return error;

		SKIP_WHILE(isspace, line, pos);
	}

	return E_SUCCESS;
}

static const char *WRONG_COMMAND =
	"Unknown command" CRLF;

static const char *ARGUMENTS_ARE_MISSING =
	"Not enough arguments" CRLF;

static const char *ARGUMENTS_ARE_INVALID =
	"Argument not a valid integer" CRLF;

static const char *UNKNOWN_PARSER_ERROR =
	"Error occurred while parsing input" CRLF;

static const char *ERROR_RUNNING_COMMAND =
	"Error occured while running command" CRLF;

static const char *USAGE =
	"Usage: ";

/*
 * Reads a command entered on the debug console and executes it
 * according to the commands array.
 *
 * Returns 1 if the console should exit and continue with normal mode.
 */
int run_command_prompt() {
	char line[LINE_LENGTH];
	unsigned int args[MAX_ARG_LEN];

	console_getline(line, LINE_LENGTH);

	console_command_t *comm = get_command(line[0]);
	if (comm) {
		error_t error = parse_args(line, args, comm->arg_length);
		if (error != E_SUCCESS) {

			if (error == E_MISSING_ARGS) {
				console_write(ARGUMENTS_ARE_MISSING);
			} else if (error == E_ARG_FORMAT) {
				console_write(ARGUMENTS_ARE_INVALID);
			} else {
				console_write(UNKNOWN_PARSER_ERROR);
			}

			console_write(USAGE);
			console_write(comm->usage);
			console_write(CRLF);

			return 0;
		}

		if (comm->handler(args) != E_SUCCESS) {
			console_write(ERROR_RUNNING_COMMAND);
			return 0;
		}

		return comm->does_exit;
	} else {
		console_write(WRONG_COMMAND);
		return 0;
	}
}

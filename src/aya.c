#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <hakomari.h>

#ifdef __GNUC__
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static MAYBE_UNUSED
#include "optparse.h"
#define OPTPARSE_HELP_IMPLEMENTATION
#define OPTPARSE_HELP_API static
#include "optparse-help.h"

#define PROG_NAME "aya"
#define quit(code) do { exit_code = code; goto quit; } while(0);

struct ask_passphrase_ctx_s
{
	hakomari_ctx_t* hakomari_ctx;
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	unsigned int last_width;
	unsigned int last_height;
};

static hakomari_error_t
std_read(void* userdata, void* buf, size_t* size)
{
	*size = fread(buf, 1, *size, userdata);
	return ferror(userdata) ? HAKOMARI_ERR_IO : HAKOMARI_OK;
}

static bool
parse_command_args(int argc, char* command_argv[], ...)
{
	va_list args;
	bool result = true;

	va_start(args, command_argv);
	for(int i = 0; i < argc; ++i)
	{
		char** argp = va_arg(args, char**);
		if(command_argv[i + 1] == NULL)
		{
			fprintf(stderr, PROG_NAME ": %s requires %d arguments", command_argv[0], argc);
			result = false;
			break;
		}

		*argp = command_argv[i + 1];
	}
	va_end(args);

	return result;
}

static bool
parse_endpoint_cmd(
	char* command_argv[],
	hakomari_endpoint_desc_t* endpoint_desc,
	hakomari_string_t* query_ptr
)
{
	const char *type, *name, *query;
	if(!parse_command_args(query_ptr ? 3 : 2, command_argv, &type, &name, &query))
	{
		return false;
	}

	memset(endpoint_desc, 0, sizeof(*endpoint_desc));

	if(false
		|| strlen(type) > sizeof(hakomari_string_t)
		|| strlen(name) > sizeof(hakomari_string_t)
		|| (query_ptr != NULL && strlen(query) > sizeof(hakomari_string_t))
	)
	{
		fprintf(stderr, PROG_NAME ": Arguments too long\n");
		return false;
	}

	strncpy(endpoint_desc->type, type, sizeof(hakomari_string_t));
	strncpy(endpoint_desc->name, name, sizeof(hakomari_string_t));
	if(query_ptr != NULL)
	{
		strncpy(*query_ptr, query, sizeof(hakomari_string_t));
	}

	return true;
}

static hakomari_error_t
ask_passphrase(void* userdata, hakomari_auth_ctx_t* auth_ctx)
{
	struct ask_passphrase_ctx_s* ctx = userdata;

	const hakomari_passphrase_screen_t* passphrase_screen;
	hakomari_error_t error = hakomari_inspect_passphrase_screen(
		auth_ctx, &passphrase_screen
	);

	if(error != HAKOMARI_OK)
	{
		const char* last_error;
		hakomari_get_last_error(ctx->hakomari_ctx, &last_error);
		fprintf(
			stderr, PROG_NAME ": Could not find passphrase screen spec: %s\n",
			last_error
		);
		return error;
	}

	if(ctx->window == NULL)
	{
		int status = SDL_CreateWindowAndRenderer(
			(int)passphrase_screen->width,
			(int)passphrase_screen->height,
			SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_OPENGL,
			&ctx->window, &ctx->renderer
		);

		if(status < 0)
		{
			fprintf(
				stderr, PROG_NAME ": Could not create window: %s\n",
				SDL_GetError()
			);

			return HAKOMARI_ERR_IO;
		}


		if(SDL_GL_SetSwapInterval(-1) < 0)
		{
			fprintf(
				stderr, PROG_NAME ": Could not set vsync: %s\n",
				SDL_GetError()
			);

			if(SDL_GL_SetSwapInterval(1) < 0)
			{
				fprintf(
					stderr, PROG_NAME ": Could not set vsync: %s\n",
					SDL_GetError()
				);
			}
		}
	}
	else
	{
		SDL_SetWindowSize(
			ctx->window,
			(int)passphrase_screen->width,
			(int)passphrase_screen->height
		);
		SDL_ShowWindow(ctx->window);
	}

	// Render passphrase screen to a texture
	if(ctx->texture != NULL
		&& (ctx->last_width != passphrase_screen->width
			|| ctx->last_height != passphrase_screen->height)
	)
	{
		SDL_DestroyTexture(ctx->texture);
		ctx->texture = NULL;
	}

	if(ctx->texture == NULL)
	{
		ctx->texture = SDL_CreateTexture(
			ctx->renderer,
			SDL_PIXELFORMAT_RGBA8888,
			SDL_TEXTUREACCESS_TARGET,
			passphrase_screen->width,
			passphrase_screen->height
		);

		if(ctx->texture == NULL)
		{
			fprintf(
				stderr, PROG_NAME ": Could not create texture: %s\n",
				SDL_GetError()
			);
		}

		ctx->last_width = passphrase_screen->width;
		ctx->last_height = passphrase_screen->height;
	}

	SDL_SetRenderTarget(ctx->renderer, ctx->texture);
	SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 0);
	SDL_RenderClear(ctx->renderer);
	SDL_SetRenderDrawColor(ctx->renderer, 255, 255, 255, 255);
	for(unsigned int y = 0; y < passphrase_screen->height; ++y)
	{
		for(unsigned int x = 0; x < passphrase_screen->width; ++x)
		{
			if(hakomari_get_pixel(passphrase_screen, x, y))
			{
				SDL_RenderDrawPoint(ctx->renderer, x, y);
			}
		}
	}
	SDL_RenderPresent(ctx->renderer);
	SDL_SetRenderTarget(ctx->renderer, NULL);

	SDL_RaiseWindow(ctx->window);

	bool running = true;
	uint32_t last_ticks = SDL_GetTicks();
	error = HAKOMARI_OK;
    while(running)
	{
		SDL_Event event;
        while(SDL_PollEvent(&event))
		{
			switch(event.type)
			{
				case SDL_MOUSEMOTION:
					error = hakomari_input_passphrase(
						auth_ctx, event.motion.x, event.motion.y, false
					);
					if(error != HAKOMARI_OK) { running = false; continue; }
					break;
				case SDL_MOUSEBUTTONDOWN:
					error = hakomari_input_passphrase(
						auth_ctx, event.button.x, event.button.y, true
					);
					if(error != HAKOMARI_OK) { running = false; continue; }
					break;
				case SDL_QUIT:
					running = false;
					break;
			}
		}

		// Keep alive by sending current mouse position
		uint32_t current_ticks = SDL_GetTicks();
		if(current_ticks - last_ticks > HAKOMARI_DEVICE_TIMEOUT / 2)
		{
			int x, y;
			SDL_GetMouseState(&x, &y);
			error = hakomari_input_passphrase(auth_ctx, x, y, false);
			if(error != HAKOMARI_OK) { running = false; continue; }

			last_ticks = current_ticks;
		}

        SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 0);
        SDL_RenderClear(ctx->renderer);
		SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
        SDL_RenderPresent(ctx->renderer);
    }

	SDL_HideWindow(ctx->window);

	return error;
}

int
main(int argc, char* argv[])
{
	(void)argc;

	int exit_code = EXIT_SUCCESS;

	struct optparse_long opts[] = {
		{"help", 'h', OPTPARSE_NONE},
		{"device", 'd', OPTPARSE_REQUIRED},
		{"no-input", 'n', OPTPARSE_NONE},
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		"INDEX", "Target a device (when multiple are plugged in)",
		NULL, "Takes no input from stdin",
	};

	const char* usage = "Usage: " PROG_NAME " [options] <command>";

	int option;
	struct optparse options;
	optparse_init(&options, argv);
	options.permute = 0;

	bool set_device = false;
	bool no_input = false;
	size_t device_index = 0;
	hakomari_ctx_t* ctx = NULL;
	hakomari_device_t* device = NULL;
	struct ask_passphrase_ctx_s ask_passphrase_ctx = { 0 };
	char* str_end;
	const char* error;

	while((option = optparse_long(&options, opts, NULL)) != -1)
	{
		switch(option)
		{
			case 'd':
				device_index = strtoull(options.optarg, &str_end, 10);
				if(*str_end != '\0')
				{
					fprintf(stderr, PROG_NAME ": Invalid device index");
					quit(EXIT_FAILURE);
				}
				set_device = true;
				break;
			case 'n':
				no_input = true;
				break;
			case 'h':
				optparse_help(usage, opts, help);
				quit(EXIT_SUCCESS);
				break;
			case '?':
				fprintf(stderr, PROG_NAME ": %s\n", options.errmsg);
				quit(EXIT_FAILURE);
				break;
			default:
				fprintf(stderr, PROG_NAME ": Unimplemented option\n");
				quit(EXIT_FAILURE);
				break;
		}
	}

	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
	{
		fprintf(stderr, PROG_NAME ": Could not init SDL: %s\n", SDL_GetError());
		quit(EXIT_FAILURE);
	}

	if(hakomari_create_context(&ctx) != HAKOMARI_OK)
	{
		fprintf(stderr, PROG_NAME ": Could not create hakomari context\n");
		quit(EXIT_FAILURE);
	}

	ask_passphrase_ctx.hakomari_ctx = ctx;
	hakomari_auth_handler_t auth_handler = {
		.userdata = &ask_passphrase_ctx,
		.ask_passphrase = ask_passphrase,
	};
	if(hakomari_set_auth_handler(ctx, &auth_handler) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, PROG_NAME ": Could not set authentication handler: %s\n", error);
		quit(EXIT_FAILURE);
	}

	size_t num_devices = 0;
	if(hakomari_enumerate_devices(ctx, &num_devices) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, PROG_NAME ": Could not enumerate devices: %s\n", error);
		quit(EXIT_FAILURE);
	}

	char* command = options.argv[options.optind];
	if(command == NULL)
	{
		fprintf(stderr, PROG_NAME ": no command given\n");
		quit(EXIT_FAILURE);
	}

	if(strcmp(command, "list-devices") == 0)
	{
		for(size_t i = 0; i < num_devices; ++i)
		{
			const hakomari_device_desc_t* device_desc = NULL;
			if(hakomari_inspect_device(ctx, i, &device_desc) != HAKOMARI_OK)
			{
				hakomari_get_last_error(ctx, &error);
				fprintf(stderr, PROG_NAME ": Could not inspect device: %s\n", error);
				quit(EXIT_FAILURE);
			}

			fprintf(stdout, "%zu: %s\n", i, device_desc->name);
		}

		quit(EXIT_SUCCESS);
	}

	if(num_devices == 0)
	{
		fprintf(stderr, PROG_NAME ": No device detected\n");
		quit(EXIT_FAILURE);
	}

	if(num_devices > 1 && !set_device)
	{
		fprintf(stderr, PROG_NAME ": Multiple devices detected, please specify one with --device\n");
		quit(EXIT_FAILURE);
	}

	if(hakomari_open_device(ctx, device_index, &device) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, PROG_NAME ": Could not open device: %s\n", error);
		quit(EXIT_FAILURE);
	}

	size_t num_endpoints;
	if(hakomari_enumerate_endpoints(device, &num_endpoints) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, PROG_NAME ": Could not enumerate endpoints: %s\n", error);
		quit(EXIT_FAILURE);
	}

	char** command_argv = &options.argv[options.optind];
	hakomari_endpoint_desc_t endpoint_desc;
	hakomari_input_t query_payload = {
		.userdata = stdin,
		.read = std_read
	};

	if(strcmp(command, "list") == 0)
	{
		for(size_t i = 0; i < num_endpoints; ++i)
		{
			const hakomari_endpoint_desc_t* endpoint_desc = NULL;
			if(hakomari_inspect_endpoint(device, i, &endpoint_desc) != HAKOMARI_OK)
			{
				hakomari_get_last_error(ctx, &error);
				fprintf(stderr, PROG_NAME ": Could not inspect endpoint: %s\n", error);
				quit(EXIT_FAILURE);
			}

			fprintf(stdout, "- type: %s\n", endpoint_desc->type);
			fprintf(stdout, "  name: %s\n", endpoint_desc->name);
		}

		quit(EXIT_SUCCESS);
	}
	else if(strcmp(command, "create") == 0)
	{
		if(!parse_endpoint_cmd(command_argv, &endpoint_desc, NULL))
		{
			quit(EXIT_FAILURE);
		}

		if(hakomari_create_endpoint(device, &endpoint_desc) != HAKOMARI_OK)
		{
			hakomari_get_last_error(ctx, &error);
			fprintf(stderr, PROG_NAME ": Could not create endpoint: %s\n", error);
			quit(EXIT_FAILURE);
		}

		quit(EXIT_SUCCESS);
	}
	else if(strcmp(command, "destroy") == 0)
	{
		if(!parse_endpoint_cmd(command_argv, &endpoint_desc, NULL))
		{
			quit(EXIT_FAILURE);
		}

		if(hakomari_destroy_endpoint(device, &endpoint_desc) != HAKOMARI_OK)
		{
			hakomari_get_last_error(ctx, &error);
			fprintf(stderr, PROG_NAME ": Could not destroy endpoint: %s\n", error);
			quit(EXIT_FAILURE);
		}

		quit(EXIT_SUCCESS);
	}
	else if(strcmp(command, "query") == 0)
	{
		hakomari_string_t query;
		if(!parse_endpoint_cmd(command_argv, &endpoint_desc, &query))
		{
			quit(EXIT_FAILURE);
		}

		hakomari_input_t* result = NULL;
		if(hakomari_query_endpoint(
			device, &endpoint_desc, query,
			no_input ? NULL : &query_payload, &result
		) != HAKOMARI_OK)
		{
			hakomari_get_last_error(ctx, &error);
			fprintf(stderr, PROG_NAME ": Could not query endpoint: %s\n", error);
			quit(EXIT_FAILURE);
		}

		size_t size;
		char buf[1024];

		while(true)
		{
			size = sizeof(buf);
			if(hakomari_read(result, buf, &size) != HAKOMARI_OK)
			{
				hakomari_get_last_error(ctx, &error);
				fprintf(stderr, PROG_NAME ": Error while reading reply: %s\n", error);
				quit(EXIT_FAILURE);
			}

			if(size == 0) { break; }

			fwrite(buf, sizeof(buf[0]), size, stdout);
		}

		quit(EXIT_SUCCESS);
	}
	else
	{
		fprintf(stderr, PROG_NAME ": Invalid command\n");
		quit(EXIT_FAILURE);
	}

quit:
	if(ask_passphrase_ctx.texture) { SDL_DestroyTexture(ask_passphrase_ctx.texture); }
	if(ask_passphrase_ctx.renderer) { SDL_DestroyRenderer(ask_passphrase_ctx.renderer); }
	if(ask_passphrase_ctx.window) { SDL_DestroyWindow(ask_passphrase_ctx.window); }
	if(device != NULL) { hakomari_close_device(device); }
	if(ctx != NULL) { hakomari_destroy_context(ctx); }
	SDL_Quit();

	return exit_code;
}

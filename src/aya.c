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

int
main(int argc, char* argv[])
{
	(void)argc;

	int exit_code = EXIT_SUCCESS;

	struct optparse_long opts[] = {
		{"help", 'h', OPTPARSE_NONE},
		{"device", 'd', OPTPARSE_REQUIRED},
		{0}
	};

	const char* help[] = {
		NULL, "Print this message",
		"INDEX", "Target a device (when multiple are plugged in)",
	};

	const char* usage = "Usage: " PROG_NAME " [options] <command>";

	int option;
	struct optparse options;
	optparse_init(&options, argv);
	options.permute = 0;

	bool set_device = false;
	size_t device_index = 0;
	hakomari_ctx_t* ctx = NULL;
	hakomari_device_t* device = NULL;
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

	if(hakomari_create_context(&ctx) != HAKOMARI_OK)
	{
		fprintf(stderr, PROG_NAME ": Could not create hakomari context\n");
		quit(EXIT_FAILURE);
	}

	size_t num_devices = 0;
	if(hakomari_enumerate_devices(ctx, &num_devices) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, "Could not enumerate devices: %s\n", error);
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
		fprintf(stderr, "Could not open device: %s\n", error);
		quit(EXIT_FAILURE);
	}

	size_t num_endpoints;
	if(hakomari_enumerate_endpoints(device, &num_endpoints) != HAKOMARI_OK)
	{
		hakomari_get_last_error(ctx, &error);
		fprintf(stderr, "Could not enumerate endpoints: %s\n", error);
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
			&query_payload, &result
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
	if(device != NULL) { hakomari_close_device(device); }
	if(ctx != NULL) { hakomari_destroy_context(ctx); }
	return exit_code;
}

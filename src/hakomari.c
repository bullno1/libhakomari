#include "hakomari.h"
#include <stdint.h>
#include <string.h>
#include <cmp/cmp.h>
#include <libserialport.h>
#define SLIPPER_API static
#define SLIPPER_IMPLEMENTATION
#include "slipper.h"

#define HAKOMARI_BUF_SIZE 1024
#define HAKOMARI_PRODUCT_PREFIX "Hakomari"

#define HAKOMARI_WITH_AUTH(OP, DEVICE, ENDPOINT, ...) \
	do { \
		hakomari_error_t error = HAKOMARI_OK; \
		bool should_retry = false; \
		bool first_time = true; \
		do { \
			error = OP(DEVICE, ENDPOINT, first_time, __VA_ARGS__); \
			first_time = false; \
			should_retry = true \
				&& error == HAKOMARI_ERR_AUTH_REQUIRED \
				&& hakomari_ask_passphrase(DEVICE, ENDPOINT) == HAKOMARI_OK; \
		} while(should_retry); \
		return error; \
	} while(0)

typedef enum hakomari_frame_type_e
{
	HAKOMARI_FRAME_REQ = 0,
	HAKOMARI_FRAME_REP,
} hakomari_frame_type_t;

struct hakomari_auth_ctx_s
{
	hakomari_device_t* device;
	const hakomari_endpoint_desc_t* endpoint;
	bool passphrase_inputed;
};

struct hakomari_ctx_s
{
	hakomari_error_t last_error;
	const char* errorstr;
	char* copied_errorstr;
	size_t num_devices;
	struct sp_port_config* port_config;
	hakomari_device_desc_t* devices;
	hakomari_auth_handler_t* auth_handler;
};

struct hakomari_mem_stream_s
{
	char* buff;
	size_t capacity;
	size_t read_pos;
	size_t write_pos;
	hakomari_input_t input;
};

struct hakomari_device_s
{
	hakomari_ctx_t* ctx;
	struct sp_port* port;
	uint32_t txid;
	uint32_t num_endpoints;
	hakomari_endpoint_desc_t* endpoints;
	slipper_ctx_t slipper;
	cmp_ctx_t cmp;
	hakomari_input_t result;
	hakomari_passphrase_screen_t passphrase_screen;
	struct hakomari_mem_stream_s payload_buff;

	uint8_t io_buf[HAKOMARI_BUF_SIZE];
};

static const char*
hakomari_errorstr(hakomari_error_t error)
{
	switch(error)
	{
		case HAKOMARI_OK:
			return "Success";
		case HAKOMARI_ERR_INVALID:
			return "Invalid argument";
		case HAKOMARI_ERR_AUTH_REQUIRED:
			return "Authentication required";
		case HAKOMARI_ERR_DENIED:
			return "Operation denied";
		case HAKOMARI_ERR_IO:
			return "IO error";
		case HAKOMARI_ERR_MEMORY:
			return "Out of memory";
		default:
			return "Sum Ting Wong";
	}
}

static hakomari_error_t
hakomari_set_last_error(
	hakomari_ctx_t* ctx, hakomari_error_t error, const char* errorstr
)
{
	ctx->errorstr = errorstr != NULL ? errorstr : hakomari_errorstr(error);
	return ctx->last_error = error;
}

hakomari_error_t
hakomari_get_last_error(hakomari_ctx_t* ctx, const char** error)
{
	if(error != NULL) { *error = ctx->errorstr; }
	return ctx->last_error;
}

static char*
hakomari_copy_sp_error(hakomari_ctx_t* ctx)
{
	char* error = sp_last_error_message();
	size_t len = strlen(error);
	ctx->copied_errorstr = realloc(ctx->copied_errorstr, len + 1);
	memcpy(ctx->copied_errorstr, error, len + 1);
	sp_free_error_message(error);
	return ctx->copied_errorstr;
}

static hakomari_error_t
hakomari_set_sp_error(hakomari_ctx_t* ctx, enum sp_return error)
{
	switch(error)
	{
		case SP_OK:
			return hakomari_set_last_error(ctx, HAKOMARI_OK, NULL);
		case SP_ERR_ARG:
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_INVALID, hakomari_copy_sp_error(ctx)
			);
		case SP_ERR_MEM:
			// Do not allocate during OOM error
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_MEMORY, NULL
			);
		case SP_ERR_FAIL:
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_IO, hakomari_copy_sp_error(ctx)
			);
		case SP_ERR_SUPP:
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_DENIED, hakomari_copy_sp_error(ctx)
			);
		default:
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_IO, "Sum Ting Wong"
			);
	}
}

hakomari_error_t
hakomari_create_context(hakomari_ctx_t** context_ptr)
{
	hakomari_ctx_t* ctx = calloc(1, sizeof(hakomari_ctx_t));
	if(ctx == NULL) { return HAKOMARI_ERR_MEMORY; }

	*context_ptr = ctx;

	enum sp_return sp_error;
	if((sp_error = sp_new_config(&ctx->port_config)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_MEMORY;
	}

	if((sp_error = sp_set_config_baudrate(ctx->port_config, 115200)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_IO;
	}

	if((sp_error = sp_set_config_bits(ctx->port_config, 8)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_IO;
	}

	if((sp_error = sp_set_config_parity(ctx->port_config, SP_PARITY_NONE)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_IO;
	}

	if((sp_error = sp_set_config_stopbits(ctx->port_config, 1)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_IO;
	}

	if((sp_error = sp_set_config_flowcontrol(ctx->port_config, SP_FLOWCONTROL_RTSCTS)) != SP_OK)
	{
		free(ctx);
		return HAKOMARI_ERR_IO;
	}

	return hakomari_set_last_error(ctx, HAKOMARI_OK, NULL);
}

void
hakomari_destroy_context(hakomari_ctx_t* context)
{
	sp_free_config(context->port_config);
	if(context->copied_errorstr) { free(context->copied_errorstr); }
	if(context->devices) { free(context->devices); }
	free(context);
}

static bool
hakomari_is_recognized_device(struct sp_port* port)
{
	return true
		&& sp_get_port_transport(port) == SP_TRANSPORT_USB
		&& strncmp(
			HAKOMARI_PRODUCT_PREFIX,
			sp_get_port_usb_product(port),
			sizeof(HAKOMARI_PRODUCT_PREFIX) - 1
		) == 0;
}

static hakomari_error_t
hakomari_enumerate_ports(
	hakomari_ctx_t* ctx, size_t* num_devices, struct sp_port** ports
)
{
	ctx->num_devices = 0;
	for(struct sp_port** itr = ports; *itr != NULL; ++itr)
	{
		struct sp_port* port = *itr;
		if(!hakomari_is_recognized_device(port)) { continue; }

		++ctx->num_devices;
	}

	*num_devices = ctx->num_devices;
	ctx->devices = realloc(ctx->devices, sizeof(*ctx->devices) * ctx->num_devices);
	if(ctx->devices == NULL)
	{
		return hakomari_set_last_error(ctx, HAKOMARI_ERR_MEMORY, NULL);
	}

	unsigned int device_index = 0;
	for(struct sp_port** itr = ports; *itr != NULL; ++itr)
	{
		struct sp_port* port = *itr;
		if(!hakomari_is_recognized_device(port)) { continue; }

		char* name = sp_get_port_description(port);
		char* sys_name = sp_get_port_name(port);
		if(false
			|| strlen(name) > sizeof(hakomari_string_t) - 1
			|| strlen(sys_name) > sizeof(hakomari_string_t) - 1
		)
		{
			return hakomari_set_last_error(
				ctx, HAKOMARI_ERR_MEMORY, "Device name is too long"
			);
		}

		hakomari_device_desc_t* desc = &ctx->devices[device_index++];
		strncpy(desc->name, name, sizeof(hakomari_string_t));
		strncpy(desc->sys_name, sys_name, sizeof(hakomari_string_t));
	}

	return hakomari_set_last_error(ctx, HAKOMARI_OK, NULL);
}

hakomari_error_t
hakomari_enumerate_devices(hakomari_ctx_t* ctx, size_t* num_devices)
{
	struct sp_port** ports;
	enum sp_return sp_error;
	if((sp_error = sp_list_ports(&ports)) != SP_OK)
	{
		return hakomari_set_sp_error(ctx, sp_error);
	}

	hakomari_error_t hakomari_error =
		hakomari_enumerate_ports(ctx, num_devices, ports);
	sp_free_port_list(ports);

	return hakomari_error;
}

hakomari_error_t
hakomari_inspect_device(
	hakomari_ctx_t* ctx, size_t index, const hakomari_device_desc_t** device
)
{
	if(index > ctx->num_devices || device == NULL)
	{
		return hakomari_set_last_error(ctx, HAKOMARI_ERR_INVALID, NULL);
	}

	*device = &ctx->devices[index];

	return hakomari_set_last_error(ctx, HAKOMARI_OK, NULL);
}

static bool
hakomari_cmp_read(cmp_ctx_t* ctx, void* data, size_t limit)
{
	hakomari_device_t* device = ctx->buf;
	size_t bytes_read = limit;
	return slipper_read(
		&device->slipper, data, &bytes_read, HAKOMARI_DEVICE_TIMEOUT
	) == SLIPPER_OK && bytes_read == limit;
}

static size_t
hakomari_cmp_write(cmp_ctx_t* ctx, const void* data, size_t count)
{
	hakomari_device_t* device = ctx->buf;
	return slipper_write(
		&device->slipper, data, count, HAKOMARI_DEVICE_TIMEOUT
	) == SLIPPER_OK ? count : 0;
}

static void
hakomari_reset_cmp(hakomari_device_t* device)
{
	cmp_init(&device->cmp, device, hakomari_cmp_read, NULL, hakomari_cmp_write);
}

static slipper_error_t
hakomari_serial_write(
	void* userdata,
	const void* data, size_t size,
	bool flush, slipper_timeout_t timeout
)
{
	hakomari_device_t* device = userdata;
	hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);

	enum sp_return error;
	if((error = sp_blocking_write(device->port, data, size, timeout) <= 0))
	{
		hakomari_set_sp_error(device->ctx, error);
		return SLIPPER_ERR_IO;
	}

	if(flush && (error = sp_drain(device->port)) != 0)
	{
		hakomari_set_sp_error(device->ctx, error);
		return SLIPPER_ERR_IO;
	}

	return SLIPPER_OK;
}

static slipper_error_t
hakomari_serial_read(
	void* userdata,
	void* data, size_t* size,
	slipper_timeout_t timeout
)
{
	hakomari_device_t* device = userdata;
	hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);

	enum sp_return bytes_read;
	if((bytes_read = sp_blocking_read_next(device->port, data, *size, timeout)) <= 0)
	{
		if(bytes_read < 0)
		{
			hakomari_set_sp_error(device->ctx, bytes_read);
			return SLIPPER_ERR_IO;
		}
		else
		{
			hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, "Device timed out");
			return SLIPPER_ERR_TIMED_OUT;
		}
	}

	*size = bytes_read;
	return SLIPPER_OK;
}

static hakomari_error_t
hakomari_device_read(void* userdata, void* buf, size_t* size)
{
	hakomari_device_t* device = userdata;
	switch(slipper_read(&device->slipper, buf, size, HAKOMARI_DEVICE_TIMEOUT))
	{
		case SLIPPER_OK:
			return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
		case SLIPPER_ERR_IO:
			return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, NULL);
		case SLIPPER_ERR_ENCODING:
			return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, "Encoding error");
		case SLIPPER_ERR_TIMED_OUT:
			return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, "Device timed out");
		default:
			return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, "Sum Ting Wong");
	}
}

static hakomari_error_t
hakomari_mem_stream_read(void* userdata, void* buf, size_t* size)
{
	struct hakomari_mem_stream_s* mem_stream = userdata;
	size_t available_bytes = mem_stream->write_pos - mem_stream->read_pos;
	size_t bytes_to_read = available_bytes < *size ? available_bytes : *size;
	if(bytes_to_read > 0)
	{
		memcpy(buf, mem_stream->buff + mem_stream->read_pos, bytes_to_read);
	}
	mem_stream->read_pos += bytes_to_read;
	*size = bytes_to_read;

	return HAKOMARI_OK;
}

static void
hakomari_mem_stream_init(struct hakomari_mem_stream_s* mem_stream)
{
	*mem_stream = (struct hakomari_mem_stream_s){
		.input = {
			.userdata = mem_stream,
			.read = hakomari_mem_stream_read
		}
	};
}

static void
hakomari_mem_stream_cleanup(struct hakomari_mem_stream_s* mem_stream)
{
	free(mem_stream->buff);
}

static void
hakomari_mem_stream_reset(struct hakomari_mem_stream_s* mem_stream)
{
	mem_stream->write_pos = 0;
}

static hakomari_input_t*
hakomari_mem_stream_as_input(struct hakomari_mem_stream_s* mem_stream)
{
	mem_stream->read_pos = 0;
	return &mem_stream->input;
}

static bool
hakomari_mem_stream_write(
	struct hakomari_mem_stream_s* mem_stream, void* buf, size_t size
)
{
	size_t required_capacity = mem_stream->write_pos + size;
	if(required_capacity > mem_stream->capacity)
	{
		mem_stream->buff = realloc(mem_stream->buff, required_capacity);
		if(mem_stream->buff == NULL) { return false; }

		mem_stream->capacity = required_capacity;
	}

	if(size > 0) { memcpy(mem_stream->buff + mem_stream->write_pos, buf, size); }
	mem_stream->write_pos += size;
	return true;
}

hakomari_error_t
hakomari_open_device(
	hakomari_ctx_t* ctx, size_t index, hakomari_device_t** device_ptr
)
{
	if(index > ctx->num_devices || device_ptr == NULL)
	{
		return hakomari_set_last_error(ctx, HAKOMARI_ERR_INVALID, NULL);
	}

	hakomari_device_desc_t* desc = &ctx->devices[index];

	enum sp_return error;
	struct sp_port* port;
	hakomari_error_t hakomari_error;
	if((error = sp_get_port_by_name(desc->sys_name, &port)) != SP_OK)
	{
		return hakomari_set_sp_error(ctx, error);
	}

	if((error = sp_open(port, SP_MODE_READ_WRITE)) != SP_OK)
	{
		hakomari_error = hakomari_set_sp_error(ctx, error);
		sp_free_port(port);
		return hakomari_error;
	}

	if((error = sp_flush(port, SP_BUF_BOTH)) != SP_OK)
	{
		hakomari_error = hakomari_set_sp_error(ctx, error);
		sp_close(port);
		sp_free_port(port);
		return hakomari_error;
	}

	if((error = sp_set_config(port, ctx->port_config)) != SP_OK)
	{
		hakomari_error = hakomari_set_sp_error(ctx, error);
		sp_close(port);
		sp_free_port(port);
		return hakomari_error;
	}

	hakomari_device_t* device = malloc(sizeof(hakomari_device_t));
	if(device == NULL)
	{
		hakomari_error = hakomari_set_last_error(ctx, HAKOMARI_ERR_MEMORY, NULL);
		sp_close(port);
		sp_free_port(port);
		return hakomari_error;
	}

	slipper_cfg_t slipper_cfg = {
		.serial = {
			.userdata = device,
			.read = hakomari_serial_read,
			.write = hakomari_serial_write,
		},
		.memory_size = HAKOMARI_BUF_SIZE,
		.memory = device->io_buf
	};

	*device = (hakomari_device_t){
		.ctx = ctx,
		.port = port,
		.result = { .userdata = device, .read = hakomari_device_read }
	};

	hakomari_reset_cmp(device);
	slipper_init(&device->slipper, &slipper_cfg);
	hakomari_mem_stream_init(&device->payload_buff);

	*device_ptr = device;
	return hakomari_set_last_error(ctx, HAKOMARI_OK, NULL);
}

void
hakomari_close_device(hakomari_device_t* device)
{
	if(device->passphrase_screen.image_data) { free(device->passphrase_screen.image_data); }
	if(device->endpoints) { free(device->endpoints); }
	sp_close(device->port);
	sp_free_port(device->port);
	hakomari_mem_stream_cleanup(&device->payload_buff);
	free(device);
}

static hakomari_error_t
hakomari_set_cmp_error(hakomari_device_t* device)
{
	if(device->ctx->last_error == HAKOMARI_OK)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, cmp_strerror(&device->cmp)
		);
	}
	else
	{
		return device->ctx->last_error;
	}
}

static hakomari_error_t
hakomari_read_endpoint_desc(
	hakomari_device_t* device, hakomari_endpoint_desc_t* desc
)
{
	uint32_t map_size;
	if(!cmp_read_map(&device->cmp, &map_size))
	{
		return hakomari_set_cmp_error(device);
	}

	if(map_size != 2)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, "Format error"
		);
	}

	memset(desc->type, 0, sizeof(desc->type));
	memset(desc->name, 0, sizeof(desc->name));

	for(uint32_t i = 0; i < map_size; ++i)
	{
		hakomari_string_t key;
		uint32_t size = sizeof(key);
		if(!cmp_read_str(&device->cmp, key, &size))
		{
			return hakomari_set_cmp_error(device);
		}

		char* value = NULL;
		if(strcmp(key, "type") == 0)
		{
			value = desc->type;
		}
		else if(strcmp(key, "name") == 0)
		{
			value = desc->name;
		}
		else
		{
			return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, "Format error");
		}

		size = sizeof(hakomari_string_t);
		if(!cmp_read_str(&device->cmp, value, &size))
		{
			return hakomari_set_cmp_error(device);
		}
	}

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

hakomari_error_t
hakomari_inspect_endpoint(
	hakomari_device_t* device,
	size_t index, const hakomari_endpoint_desc_t** endpoint
)
{
	if(index > device->num_endpoints || device == NULL)
	{
		return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_INVALID, NULL);
	}

	*endpoint = &device->endpoints[index];

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

static hakomari_error_t
hakomari_begin_query(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* desc,
	const hakomari_string_t query
)
{
	hakomari_reset_cmp(device);

	if(slipper_begin_write(&device->slipper, HAKOMARI_DEVICE_TIMEOUT) != SLIPPER_OK)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, "Error writing message start"
		);
	}

	if(false
		|| !cmp_write_array(&device->cmp, 4)
		|| !cmp_write_u8(&device->cmp, HAKOMARI_FRAME_REQ)
		|| !cmp_write_u32(&device->cmp, device->txid++)
		|| !cmp_write_str(&device->cmp, query, strlen(query))
	)
	{
		return hakomari_set_cmp_error(device);
	}

	if(desc)
	{
		if(false
			|| !cmp_write_array(&device->cmp, 2)
			|| !cmp_write_str(&device->cmp, desc->type, strlen(desc->type))
			|| !cmp_write_str(&device->cmp, desc->name, strlen(desc->name))
		)
		{
			return hakomari_set_cmp_error(device);
		}
	}
	else
	{
		if(!cmp_write_nil(&device->cmp))
		{
			return hakomari_set_cmp_error(device);
		}
	}

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

static hakomari_error_t
hakomari_end_query(hakomari_device_t* device, hakomari_input_t** result)
{
	hakomari_error_t status = HAKOMARI_OK;
	slipper_error_t error;
	if((error = slipper_end_write(&device->slipper, HAKOMARI_DEVICE_TIMEOUT)) != 0)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, slipper_errorstr(error)
		);
	}

	while(true)
	{
		if(slipper_begin_read(
			&device->slipper, HAKOMARI_DEVICE_TIMEOUT
		) != SLIPPER_OK)
		{
			return device->ctx->last_error;
		}

		uint32_t size;
		if(!cmp_read_array(&device->cmp, &size))
		{
			return hakomari_set_cmp_error(device);
		}

		if(size != 3)
		{
			return hakomari_set_last_error(
				device->ctx, HAKOMARI_ERR_IO, "Format error"
			);
		}

		uint8_t type;
		if(!cmp_read_u8(&device->cmp, &type))
		{
			return hakomari_set_cmp_error(device);
		}

		if(type != HAKOMARI_FRAME_REP)
		{
			return hakomari_set_last_error(
				device->ctx, HAKOMARI_ERR_IO, "Format error"
			);
		}

		uint32_t txid;
		if(!cmp_read_u32(&device->cmp, &txid))
		{
			return hakomari_set_cmp_error(device);
		}

		if(txid != (device->txid - 1))
		{
			if(slipper_end_read(
				&device->slipper, HAKOMARI_DEVICE_TIMEOUT
			) != SLIPPER_OK)
			{
				return hakomari_set_last_error(
					device->ctx, HAKOMARI_ERR_IO, "Error while reading reply"
				);
			}

			hakomari_reset_cmp(device);
			continue;
		}

		uint8_t result;
		if(!cmp_read_u8(&device->cmp, &result))
		{
			return hakomari_set_cmp_error(device);
		}

		status = (hakomari_error_t)result;
		break;
	}

	if(result)
	{
		*result = status == HAKOMARI_OK ? &device->result : NULL;
	}

	return hakomari_set_last_error(device->ctx, status, NULL);
}

static hakomari_error_t
hakomari_send_payload(
	hakomari_device_t* device, bool first_time, hakomari_input_t* payload
)
{
	char buf[HAKOMARI_BUF_SIZE];
	size_t size;

	hakomari_input_t* source = NULL;
	if(first_time)
	{
		source = payload;
		hakomari_mem_stream_reset(&device->payload_buff);
	}
	else
	{
		source = hakomari_mem_stream_as_input(&device->payload_buff);
	}

	do
	{
		size = HAKOMARI_BUF_SIZE;
		switch(hakomari_read(source, buf, &size))
		{
			case HAKOMARI_OK:
				if(first_time &&
					!hakomari_mem_stream_write(&device->payload_buff, buf, size)
				)
				{
					return hakomari_set_last_error(
						device->ctx, HAKOMARI_ERR_MEMORY, NULL
					);
				}

				if(slipper_write(
					&device->slipper, buf, size, HAKOMARI_DEVICE_TIMEOUT
				) != SLIPPER_OK)
				{
					return hakomari_set_last_error(
						device->ctx, HAKOMARI_ERR_IO, "Error while sending payload"
					);
				}
				break;
			case HAKOMARI_ERR_IO:
				return hakomari_set_last_error(
					device->ctx, HAKOMARI_ERR_IO, "Error while reading payload"
				);
			default:
				return hakomari_set_last_error(
					device->ctx, HAKOMARI_ERR_INVALID, "Invalid payload stream"
				);
		}
	} while(size);

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

static hakomari_error_t
hakomari_query_endpoint_authenticated(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* desc,
	bool first_time,
	const hakomari_string_t query, hakomari_input_t* payload,
	hakomari_input_t** result
)
{
	hakomari_error_t error;
	if((error = hakomari_begin_query(device, desc, query)) != HAKOMARI_OK)
	{
		return error;
	}

	if(true
		&& payload != NULL
		&& (error = hakomari_send_payload(device, first_time, payload)) != HAKOMARI_OK
	)
	{
		return error;
	}

	return hakomari_end_query(device, result);
}

static hakomari_error_t
hakomari_ask_passphrase(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint
)
{
	hakomari_error_t error;
	hakomari_auth_handler_t* auth_handler = device->ctx->auth_handler;

	if(auth_handler == NULL)
	{
		return HAKOMARI_ERR_AUTH_REQUIRED;
	}

	// Get specification for the passphrase input screen
	error = hakomari_query_endpoint_authenticated(
		device, endpoint, true, "@get-passphrase-screen", NULL, NULL
	);
	if(error != HAKOMARI_OK) { return error; }

	uint32_t map_size;
	if(!cmp_read_map(&device->cmp, &map_size))
	{
		return hakomari_set_cmp_error(device);
	}

	if(map_size != 3)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, "Format error"
		);
	}

	hakomari_passphrase_screen_t* passphrase_screen = &device->passphrase_screen;
	passphrase_screen->width = passphrase_screen->height = 0;
	uint32_t image_data_size = 0;

	for(uint32_t i = 0; i < map_size; ++i)
	{
		hakomari_string_t key;
		uint32_t size = sizeof(key);

		if(!cmp_read_str(&device->cmp, key, &size))
		{
			return hakomari_set_cmp_error(device);
		}

		if(strcmp(key, "width") == 0 || strcmp(key, "height") == 0)
		{
			uint32_t dim;
			if(!cmp_read_uint(&device->cmp, &dim))
			{
				return hakomari_set_cmp_error(device);
			}

			if(strcmp(key, "width") == 0)
			{
				passphrase_screen->width = dim;
			}
			else
			{
				passphrase_screen->height = dim;
			}
		}
		else if(strcmp(key, "image_data") == 0)
		{
			if(!cmp_read_bin_size(&device->cmp, &image_data_size))
			{
				return hakomari_set_cmp_error(device);
			}

			passphrase_screen->image_data = realloc(
				passphrase_screen->image_data, image_data_size
			);

			if(!device->cmp.read(
				&device->cmp, passphrase_screen->image_data, image_data_size
			))
			{
				return hakomari_set_cmp_error(device);
			}
		}
		else
		{
			return hakomari_set_last_error(
				device->ctx, HAKOMARI_ERR_IO, "Format error"
			);
		}
	}

	if(passphrase_screen->width * passphrase_screen->height / 8 != image_data_size)
	{
		return hakomari_set_last_error(
			device->ctx, HAKOMARI_ERR_IO, "Format error"
		);
	}

	// Actual passphrase prompt
	hakomari_auth_ctx_t auth_ctx = {
		.device = device,
		.endpoint = endpoint,
		.passphrase_inputed = false,
	};

	error = hakomari_begin_query(device, endpoint, "@input-passphrase");
	if(error != HAKOMARI_OK) { return error; }

	if(slipper_flush(&device->slipper, HAKOMARI_DEVICE_TIMEOUT) != SLIPPER_OK)
	{
		return HAKOMARI_ERR_IO;
	}

	error = auth_handler->ask_passphrase(auth_handler->userdata, &auth_ctx);
	if(error != HAKOMARI_OK) { return error; }

	// Mark end of input stream
	if(!cmp_write_nil(&device->cmp)) { return hakomari_set_cmp_error(device); }

	error = hakomari_end_query(device, NULL);
	if(error != HAKOMARI_OK) { return error; }

	return hakomari_set_last_error(
		device->ctx,
		auth_ctx.passphrase_inputed ? HAKOMARI_OK : HAKOMARI_ERR_AUTH_REQUIRED,
		NULL
	);
}

hakomari_error_t
hakomari_query_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint,
	const hakomari_string_t query, hakomari_input_t* payload,
	hakomari_input_t** result
)
{
	HAKOMARI_WITH_AUTH(
		hakomari_query_endpoint_authenticated,
		device, endpoint, query, payload, result
	);
}

hakomari_error_t
hakomari_inspect_passphrase_screen(
	hakomari_auth_ctx_t* auth_ctx,
	const hakomari_passphrase_screen_t** screen_ptr
)
{
	if(screen_ptr == NULL || auth_ctx == NULL)
	{
		return hakomari_set_last_error(
			auth_ctx->device->ctx, HAKOMARI_ERR_INVALID, NULL
		);
	}

	*screen_ptr = &auth_ctx->device->passphrase_screen;

	return hakomari_set_last_error(auth_ctx->device->ctx, HAKOMARI_OK, NULL);
}

hakomari_error_t
hakomari_input_passphrase(
	hakomari_auth_ctx_t* auth_ctx, unsigned int x, unsigned int y, bool down
)
{
	hakomari_device_t* device = auth_ctx->device;
	auth_ctx->passphrase_inputed |= down;

	if(false
		|| !cmp_write_array(&device->cmp, 3)
		|| !cmp_write_uint(&device->cmp, x)
		|| !cmp_write_uint(&device->cmp, y)
		|| !cmp_write_bool(&device->cmp, down)
	)
	{
		return hakomari_set_cmp_error(device);
	}

	if(slipper_flush(&device->slipper, HAKOMARI_DEVICE_TIMEOUT) != SLIPPER_OK)
	{
		return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_IO, NULL);
	}

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

hakomari_error_t
hakomari_enumerate_endpoints(hakomari_device_t* device, size_t* num_endpoints)
{
	if(hakomari_query_endpoint(
		device, NULL, "@enumerate", NULL, NULL
	) != HAKOMARI_OK)
	{
		return device->ctx->last_error;
	}

	if(!cmp_read_array(&device->cmp, &device->num_endpoints))
	{
		return hakomari_set_cmp_error(device);
	}

	device->endpoints = realloc(
		device->endpoints, device->num_endpoints * sizeof(hakomari_endpoint_desc_t)
	);
	if(device->endpoints == NULL)
	{
		return hakomari_set_last_error(device->ctx, HAKOMARI_ERR_MEMORY, NULL);
	}

	for(uint32_t i = 0; i < device->num_endpoints; ++i)
	{
		hakomari_error_t error;
		if((error = hakomari_read_endpoint_desc(device, &device->endpoints[i])) != 0)
		{
			return error;
		}
	}

	*num_endpoints = device->num_endpoints;

	return hakomari_set_last_error(device->ctx, HAKOMARI_OK, NULL);
}

static hakomari_error_t
hakomari_create_or_destroy_endpoint_authenticated(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint,
	bool first_time, const char* op
)
{
	(void)first_time;

	hakomari_error_t error;
	if((error = hakomari_begin_query(device, NULL, op)) != HAKOMARI_OK)
	{
		return error;
	}

	if(false
		|| !cmp_write_map(&device->cmp, 2)
		|| !cmp_write_str(&device->cmp, "type", sizeof("type") - 1)
		|| !cmp_write_str(&device->cmp, endpoint->type, strlen(endpoint->type))
		|| !cmp_write_str(&device->cmp, "name", sizeof("name") - 1)
		|| !cmp_write_str(&device->cmp, endpoint->name, strlen(endpoint->name))
	)
	{
		return hakomari_set_cmp_error(device);
	}

	return hakomari_end_query(device, NULL);
}

static hakomari_error_t
hakomari_create_or_destroy_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint,
	const char* op
)
{
	HAKOMARI_WITH_AUTH(
		hakomari_create_or_destroy_endpoint_authenticated,
		device, endpoint, op
	);
}

hakomari_error_t
hakomari_create_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint
)
{
	return hakomari_create_or_destroy_endpoint(device, endpoint, "@create");
}

hakomari_error_t
hakomari_destroy_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* endpoint
)
{
	return hakomari_create_or_destroy_endpoint(device, endpoint, "@destroy");
}

hakomari_error_t
hakomari_set_auth_handler(
	hakomari_ctx_t* context, hakomari_auth_handler_t* auth_handler
)
{
	if(auth_handler != NULL && auth_handler->ask_passphrase == NULL)
	{
		return hakomari_set_last_error(context, HAKOMARI_ERR_INVALID, NULL);
	}

	context->auth_handler = auth_handler;

	return hakomari_set_last_error(context, HAKOMARI_OK, NULL);
}

#ifndef HAKOMARI_H
#define HAKOMARI_H

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define HAKOMARI_DEVICE_TIMEOUT 10000

typedef struct hakomari_ctx_s hakomari_ctx_t;
typedef struct hakomari_device_s hakomari_device_t;
typedef struct hakomari_device_desc_s hakomari_device_desc_t;
typedef struct hakomari_endpoint_desc_s hakomari_endpoint_desc_t;
typedef struct hakomari_input_s hakomari_input_t;
typedef struct hakomari_auth_handler_s hakomari_auth_handler_t;
typedef struct hakomari_auth_ctx_s hakomari_auth_ctx_t;
typedef struct hakomari_passphrase_screen_s hakomari_passphrase_screen_t;

typedef char hakomari_string_t[128];

typedef enum hakomari_error_e
{
	HAKOMARI_OK,
	HAKOMARI_ERR_INVALID,
	HAKOMARI_ERR_MEMORY,
	HAKOMARI_ERR_AUTH_REQUIRED,
	HAKOMARI_ERR_DENIED,
	HAKOMARI_ERR_IO,
} hakomari_error_t;

struct hakomari_device_desc_s
{
	/// User-friendly name
	hakomari_string_t name;

	/// System name (e.g: "COM1", "/dev/ttyACM0")
	hakomari_string_t sys_name;
};

struct hakomari_endpoint_desc_s
{
	/// Endpoint's type (e.g: "@provider", "GPG", "XMR")
	hakomari_string_t type;

	/// User-defined name (e.g: "My HODL")
	hakomari_string_t name;
};

struct hakomari_input_s
{
	void* userdata;
	hakomari_error_t(*read)(void* userdata, void* buf, size_t* size);
};

struct hakomari_auth_handler_s
{
	void* userdata;
	hakomari_error_t(*ask_passphrase)(
		void* userdata, hakomari_auth_ctx_t* auth_ctx
	);
};

struct hakomari_passphrase_screen_s
{
	unsigned int width;
	unsigned int height;
	void* image_data;
};

hakomari_error_t
hakomari_create_context(hakomari_ctx_t** context_ptr);

void
hakomari_destroy_context(hakomari_ctx_t* context);

hakomari_error_t
hakomari_set_auth_handler(
	hakomari_ctx_t* context, hakomari_auth_handler_t* auth_handler
);

hakomari_error_t
hakomari_get_last_error(hakomari_ctx_t* context, const char** error);

hakomari_error_t
hakomari_enumerate_devices(hakomari_ctx_t* ctx, size_t* num_devices);

hakomari_error_t
hakomari_inspect_device(
	hakomari_ctx_t* ctx, size_t index, const hakomari_device_desc_t** desc
);

hakomari_error_t
hakomari_open_device(
	hakomari_ctx_t* ctx, size_t index, hakomari_device_t** device
);

void
hakomari_close_device(hakomari_device_t* device);

hakomari_error_t
hakomari_enumerate_endpoints(hakomari_device_t* device, size_t* num_endpoints);

hakomari_error_t
hakomari_inspect_endpoint(
	hakomari_device_t* device,
	size_t index, const hakomari_endpoint_desc_t** desc
);

hakomari_error_t
hakomari_create_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* desc
);

hakomari_error_t
hakomari_destroy_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* desc
);

hakomari_error_t
hakomari_query_endpoint(
	hakomari_device_t* device, const hakomari_endpoint_desc_t* desc,
	const hakomari_string_t query, hakomari_input_t* payload,
	hakomari_input_t** result
);

hakomari_error_t
hakomari_inspect_passphrase_screen(
	hakomari_auth_ctx_t* auth_ctx,
	const hakomari_passphrase_screen_t** screen_ptr
);

hakomari_error_t
hakomari_input_passphrase(
	hakomari_auth_ctx_t* auth_ctx,
	unsigned int x, unsigned int y, bool down
);

static inline hakomari_error_t
hakomari_read(hakomari_input_t* stream, void* buf, size_t* size)
{
	return stream->read(stream->userdata, buf, size);
}

static inline bool
hakomari_get_pixel(
	const hakomari_passphrase_screen_t* screen,
	unsigned int x, unsigned int y
)
{
	if(x >= screen->width || y >= screen->height) { return false; }

	uint8_t byte = ((uint8_t*)(screen->image_data))[(x + y * screen->width) / 8];
	return (byte >> (x % 8)) & 1;
}

#endif

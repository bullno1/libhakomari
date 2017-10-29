#ifndef SLIPPER_H
#define SLIPPER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#define SLIPPER_INFINITY UINT_MAX

#ifndef SLIPPER_API
#define SLIPPER_API
#endif

typedef struct slipper_cfg_s slipper_cfg_t;
typedef struct slipper_ctx_s slipper_ctx_t;
typedef struct slipper_serial_s slipper_serial_t;
typedef unsigned int slipper_timeout_t;

typedef enum slipper_error_e
{
	SLIPPER_OK,
	SLIPPER_ERR_IO,
	SLIPPER_ERR_ENCODING,
	SLIPPER_ERR_TIMED_OUT,
} slipper_error_t;

struct slipper_serial_s
{
	void* userdata;

	/**
	 * Write data out to the pipe, optionally flushes output buffer.
	 * Do not return until write finishes or an error occured.
	 */
	slipper_error_t(*write)(
		void* userdata,
		const void* data, size_t size,
		bool flush, slipper_timeout_t timeout
	);

	/**
	 * Read data from the pipe.
	 * Return as soon as an error happens or any data is available.
	 * Write number of bytes read back to size.
	 */
	slipper_error_t(*read)(
		void* userdata, void* data, size_t* size, slipper_timeout_t timeout
	);
};

struct slipper_cfg_s
{
	slipper_serial_t serial;
	size_t memory_size;
	void* memory;
};

struct slipper_ctx_s
{
	slipper_cfg_t cfg;
	size_t cursor;
	size_t read_limit;
};

static inline void
slipper_init(slipper_ctx_t* ctx, slipper_cfg_t* cfg)
{
	*ctx = (slipper_ctx_t){ .cfg = *cfg };
}

SLIPPER_API slipper_error_t
slipper_begin_write(slipper_ctx_t* ctx, slipper_timeout_t timeout);

SLIPPER_API slipper_error_t
slipper_write(
	slipper_ctx_t* ctx, const void* data, size_t size,
	slipper_timeout_t timeout
);

SLIPPER_API slipper_error_t
slipper_end_write(slipper_ctx_t* ctx, slipper_timeout_t timeout);

SLIPPER_API slipper_error_t
slipper_begin_read(slipper_ctx_t* ctx, slipper_timeout_t timeout);

SLIPPER_API slipper_error_t
slipper_read(
	slipper_ctx_t* ctx, void* data, size_t* size, slipper_timeout_t timeout
);

SLIPPER_API slipper_error_t
slipper_end_read(slipper_ctx_t* ctx, slipper_timeout_t timeout);

SLIPPER_API const char*
slipper_errorstr(slipper_error_t error);

#ifdef SLIPPER_IMPLEMENTATION

#include <stdint.h>
#include <memory.h>

#define SLIPPER_MSG_END 0xC0
#define SLIPPER_MSG_ESC 0xDB
#define SLIPPER_MSG_ESC_END 0xDC
#define SLIPPER_MSG_ESC_ESC 0xDD
static const uint8_t SLIPPER_MSG_ESCAPED_END[] = { SLIPPER_MSG_ESC, SLIPPER_MSG_ESC_END };
static const uint8_t SLIPPER_MSG_ESCAPED_ESC[] = { SLIPPER_MSG_ESC, SLIPPER_MSG_ESC_ESC };

static slipper_error_t
slipper_ensure_read_buf(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	if(ctx->cursor < ctx->read_limit) { return SLIPPER_OK; }

	ctx->cursor = 0;
	ctx->read_limit = ctx->cfg.memory_size;

	slipper_error_t error;
	if((error = ctx->cfg.serial.read(
		ctx->cfg.serial.userdata,
		ctx->cfg.memory, &ctx->read_limit,
		timeout
	)) != SLIPPER_OK)
	{
		return error;
	}

	return SLIPPER_OK;
}

static slipper_error_t
slipper_read_byte(slipper_ctx_t* ctx, uint8_t* byte, slipper_timeout_t timeout)
{
	slipper_error_t error;
	if((error = slipper_ensure_read_buf(ctx, timeout)) != SLIPPER_OK)
	{
		return error;
	}

	*byte = ((uint8_t*)ctx->cfg.memory)[ctx->cursor++];
	return SLIPPER_OK;
}

static slipper_error_t
slipper_flush(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	size_t num_bytes = ctx->cursor;
	ctx->cursor = 0;

	return ctx->cfg.serial.write(
		ctx->cfg.serial.userdata, ctx->cfg.memory, num_bytes, true, timeout
	);
}

static slipper_error_t
slipper_write_escaped(
	slipper_ctx_t* ctx,
	const void* data, size_t size,
	slipper_timeout_t timeout
)
{
	const uint8_t* write_buf = data;
	slipper_error_t error;

	while(size)
	{
		if(ctx->cursor == 0 && size > ctx->cfg.memory_size)
		{
			if((error = ctx->cfg.serial.write(
				ctx->cfg.serial.userdata, write_buf, size, true, timeout
			)) != SLIPPER_OK)
			{
				return error;
			}

			write_buf += size;
			size = 0;
		}
		else
		{
			size_t space_left = ctx->cfg.memory_size - ctx->cursor;
			size_t write_size = size < space_left ? size : space_left;

			memcpy(
				(uint8_t*)ctx->cfg.memory + ctx->cursor, write_buf, write_size
			);

			ctx->cursor += write_size;
			size -= write_size;
			write_buf += write_size;

			if(
				ctx->cursor == ctx->cfg.memory_size
				&& (error = slipper_flush(ctx, timeout)) != SLIPPER_OK
			)
			{
				return error;
			}
		}
	}

	return SLIPPER_OK;
}

static slipper_error_t
slipper_write_delimiter(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	uint8_t header[] = { SLIPPER_MSG_END };
	size_t size = sizeof(header);
	return slipper_write_escaped(ctx, header, size, timeout);
}


const char*
slipper_errorstr(slipper_error_t error)
{
	switch(error)
	{
		case SLIPPER_OK:
			return "No error";
		case SLIPPER_ERR_IO:
			return "IO error";
		case SLIPPER_ERR_ENCODING:
			return "Encoding error";
		case SLIPPER_ERR_TIMED_OUT:
			return "Timed out";
		default:
			return "Sum Ting Wong";
	}
}

slipper_error_t
slipper_begin_write(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	ctx->cursor = 0;
	return slipper_write_delimiter(ctx, timeout);
}

slipper_error_t
slipper_end_write(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	slipper_error_t error;

	if((error = slipper_write_delimiter(ctx, timeout)) != SLIPPER_OK)
	{
		return error;
	}

	if((error = slipper_flush(ctx, timeout)) != SLIPPER_OK)
	{
		return error;
	}

	return SLIPPER_OK;
}

slipper_error_t
slipper_write(
	slipper_ctx_t* ctx, const void* data, size_t size,
	slipper_timeout_t timeout
)
{
	for(size_t i = 0; i < size; ++i)
	{
		uint8_t byte = ((const uint8_t*)data)[i];
		const uint8_t* bytes;
		size_t data_size;

		switch(byte)
		{
			case SLIPPER_MSG_ESC:
				bytes = SLIPPER_MSG_ESCAPED_ESC;
				data_size = sizeof(SLIPPER_MSG_ESCAPED_ESC);
				break;
			case SLIPPER_MSG_END:
				bytes = SLIPPER_MSG_ESCAPED_END;
				data_size = sizeof(SLIPPER_MSG_ESCAPED_END);
				break;
			default:
				bytes = &byte;
				data_size = sizeof(byte);
				break;
		}

		slipper_error_t error;
		if((error = slipper_write_escaped(ctx, bytes, data_size, timeout)) != SLIPPER_OK)
		{
			return error;
		}
	}

	return SLIPPER_OK;
}

slipper_error_t
slipper_end_read(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	uint8_t byte;
	slipper_error_t error;

	do
	{
		if((error = slipper_read_byte(ctx, &byte, timeout)) != SLIPPER_OK)
		{
			return error;
		}
	} while(byte != SLIPPER_MSG_END);

	return SLIPPER_OK;
}

slipper_error_t
slipper_begin_read(slipper_ctx_t* ctx, slipper_timeout_t timeout)
{
	slipper_error_t error;

	ctx->cursor = 0;
	ctx->read_limit = 0;

	if((error = slipper_end_read(ctx, timeout)) != SLIPPER_OK)
	{
		return error;
	}

	uint8_t byte;
	do
	{
		if((error = slipper_read_byte(ctx, &byte, timeout)) != SLIPPER_OK)
		{
			return error;
		}
	} while(byte == SLIPPER_MSG_END);

	--ctx->cursor;

	return SLIPPER_OK;
}

slipper_error_t
slipper_read(
	slipper_ctx_t* ctx, void* data, size_t* size, slipper_timeout_t timeout
)
{
	uint8_t* read_buf = data;
	size_t bytes_read = 0;
	size_t num_bytes = *size;

	while(bytes_read < num_bytes)
	{
		uint8_t byte;
		slipper_error_t error;

		if((error = slipper_read_byte(ctx, &byte, timeout)) != SLIPPER_OK)
		{
			return error;
		}

		switch(byte)
		{
			case SLIPPER_MSG_END:
				--ctx->cursor; // Make end status sticky
				*size = bytes_read;
				return SLIPPER_OK;
			case SLIPPER_MSG_ESC:
				if((error = slipper_read_byte(ctx, &byte, timeout)) != SLIPPER_OK)
				{
					return error;
				}

				switch(byte)
				{
					case SLIPPER_MSG_ESC_END:
						byte = SLIPPER_MSG_END;
						break;
					case SLIPPER_MSG_ESC_ESC:
						byte = SLIPPER_MSG_ESC;
						break;
					default:
						return SLIPPER_ERR_ENCODING;
				}
				break;
			default:
				break;
		}

		*read_buf = byte;
		++read_buf;
		++bytes_read;
	}

	*size = bytes_read;
	return SLIPPER_OK;
}

#endif

#endif

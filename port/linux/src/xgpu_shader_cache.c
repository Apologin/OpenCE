/*
XGPU_SHADER_CACHE.C

Shader warm-up: the renderer makes a program for each pair of vertex and
pixel shader the first time a draw needs it (d3d8_gl.c program_get), and
linking one is slow where the browser compiles it again for Direct3D
(WebGL on Windows): the game hitched each time it met a new surface, weapon
or effect, most of all early in a game. So every program linked is
recorded (its two shaders' GLSL), in shader_cache.bin in the data root,
and at start-up (gl_initialize, behind the loading screen) the programs
recorded before are compiled and linked again, all at once, where the
browser's parallel compile (KHR_parallel_shader_compile) can share the
work out. When a draw then asks for the same pair, it is given the program
made then instead of linking one mid-frame. The first game on a map still
records what it meets; the next ones start warm.

shader_cache.bin: "XSC1", then records: 'S', the type (u32), the source's
hash (u64), its length (u32) and the source; 'P', the vertex and the pixel
shader's hashes (u64 each). Little-endian. A file that does not read is
started again.

A warm program is handed out once: two draws' program cache entries
(program_entry) keep their own uniform values, so they never share one.
*/

#include "xgpu.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_FILE_NAME "shader_cache.bin"
#define CACHE_MAGIC "XSC1"
/* a file this large is not ours (a few thousand programs are a few MB) */
#define MAXIMUM_CACHE_FILE (64 * 1024 * 1024)
#define MAXIMUM_SOURCE (1024 * 1024)

typedef unsigned long long hash64;

struct cached_shader
{
	hash64 hash;
	GLenum type;
	char *source;
};

struct cached_program
{
	hash64 vertex;
	hash64 fragment;
	/* warmed up and not handed out yet, else 0 */
	GLuint program;
};

static struct
{
	BOOL loaded;
	/* the file has its magic (append mode's position says nothing before
	the first write, so it can't tell) */
	BOOL started;
	char path[1024];
	/* every shader source known (from the file, or compiled this run) */
	struct cached_shader *shaders;
	long shader_count, shader_capacity;
	/* every program pair known (recorded, or linked this run) */
	struct cached_program *programs;
	long program_count, program_capacity;
	/* this run's shader names: GL name -> the hash of its source (0: none) */
	hash64 *names;
	unsigned long name_capacity;
	/* which shaders and pairs the file has (so each is written once) */
	unsigned char *shader_written;
	unsigned char *program_written;
} cache;

static hash64 source_hash(GLenum type, const char *source)
{
	hash64 hash = 1469598103934665603ULL ^ (hash64)type;

	for (; *source; source++)
		hash = (hash ^ (unsigned char)*source) * 1099511628211ULL;
	return hash ? hash : 1;
}

static long shader_find(hash64 hash)
{
	long index;

	for (index = 0; index < cache.shader_count; index++)
	{
		if (cache.shaders[index].hash == hash)
			return index;
	}
	return -1;
}

static long program_find(hash64 vertex, hash64 fragment)
{
	long index;

	for (index = 0; index < cache.program_count; index++)
	{
		if (cache.programs[index].vertex == vertex && cache.programs[index].fragment == fragment)
			return index;
	}
	return -1;
}

static BOOL grow(void **items, long *capacity, long count, size_t size, unsigned char **written)
{
	void *bigger;
	unsigned char *bigger_written;
	long wanted;

	if (count < *capacity)
		return TRUE;
	wanted = *capacity ? *capacity * 2 : 256;
	bigger = realloc(*items, (size_t)wanted * size);
	if (!bigger)
		return FALSE;
	*items = bigger;
	bigger_written = realloc(*written, (size_t)wanted);
	if (!bigger_written)
		return FALSE;
	memset(bigger_written + *capacity, 0, (size_t)(wanted - *capacity));
	*written = bigger_written;
	*capacity = wanted;
	return TRUE;
}

static long shader_add(hash64 hash, GLenum type, const char *source, size_t length)
{
	long index = shader_find(hash);
	char *copy;

	if (index >= 0)
		return index;
	if (!grow((void **)&cache.shaders, &cache.shader_capacity, cache.shader_count, sizeof(*cache.shaders),
		&cache.shader_written))
		return -1;
	copy = malloc(length + 1);
	if (!copy)
		return -1;
	memcpy(copy, source, length);
	copy[length] = 0;
	cache.shaders[cache.shader_count].hash = hash;
	cache.shaders[cache.shader_count].type = type;
	cache.shaders[cache.shader_count].source = copy;
	return cache.shader_count++;
}

static long program_add(hash64 vertex, hash64 fragment)
{
	long index = program_find(vertex, fragment);

	if (index >= 0)
		return index;
	if (!grow((void **)&cache.programs, &cache.program_capacity, cache.program_count, sizeof(*cache.programs),
		&cache.program_written))
		return -1;
	cache.programs[cache.program_count].vertex = vertex;
	cache.programs[cache.program_count].fragment = fragment;
	cache.programs[cache.program_count].program = 0;
	return cache.program_count++;
}

static unsigned long read_u32(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8) | ((unsigned long)bytes[2] << 16) |
		((unsigned long)bytes[3] << 24);
}

static hash64 read_u64(const unsigned char *bytes)
{
	return (hash64)read_u32(bytes) | ((hash64)read_u32(bytes + 4) << 32);
}

static void write_u32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

static void write_u64(unsigned char *bytes, hash64 value)
{
	write_u32(bytes, (unsigned long)value);
	write_u32(bytes + 4, (unsigned long)(value >> 32));
}

/* the file's records, into the tables (marked as written); FALSE when it
does not read (it is then started again) */
static BOOL cache_read(void)
{
	FILE *file = fopen(cache.path, "rb");
	unsigned char *bytes;
	long size, at;
	BOOL ok = TRUE;

	if (!file)
		return TRUE;
	cache.started = TRUE;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size < 4 || size > MAXIMUM_CACHE_FILE || !(bytes = malloc((size_t)size)))
	{
		fclose(file);
		/* (an empty file: the magic goes in with the first record) */
		if (size == 0)
			cache.started = FALSE;
		return size == 0;
	}
	if ((long)fread(bytes, 1, (size_t)size, file) != size || memcmp(bytes, CACHE_MAGIC, 4))
		ok = FALSE;
	fclose(file);
	for (at = 4; ok && at < size; )
	{
		if (bytes[at] == 'S' && at + 17 <= size)
		{
			GLenum type = (GLenum)read_u32(bytes + at + 1);
			hash64 hash = read_u64(bytes + at + 5);
			unsigned long length = read_u32(bytes + at + 13);
			long index;

			if (length > MAXIMUM_SOURCE || at + 17 + (long)length > size ||
				(type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER))
			{
				ok = FALSE;
				break;
			}
			index = shader_add(hash, type, (const char *)bytes + at + 17, length);
			if (index >= 0)
				cache.shader_written[index] = 1;
			at += 17 + (long)length;
		}
		else if (bytes[at] == 'P' && at + 17 <= size)
		{
			long index = program_add(read_u64(bytes + at + 1), read_u64(bytes + at + 9));

			if (index >= 0)
				cache.program_written[index] = 1;
			at += 17;
		}
		else
		{
			/* (a record cut short by a write that did not finish: the rest
			is kept, the file is written again whole below) */
			ok = FALSE;
		}
	}
	free(bytes);
	return ok;
}

/* an empty file again (after one that did not read): what it had that did
read is appended again by the caller */
static void cache_rewrite(void)
{
	FILE *file = fopen(cache.path, "wb");

	if (!file)
		return;
	fwrite(CACHE_MAGIC, 1, 4, file);
	fclose(file);
	cache.started = TRUE;
	if (cache.shader_written)
		memset(cache.shader_written, 0, (size_t)cache.shader_capacity);
	if (cache.program_written)
		memset(cache.program_written, 0, (size_t)cache.program_capacity);
}

static void cache_append_shader(FILE *file, long index)
{
	unsigned char header[17];
	size_t length = strlen(cache.shaders[index].source);

	header[0] = 'S';
	write_u32(header + 1, cache.shaders[index].type);
	write_u64(header + 5, cache.shaders[index].hash);
	write_u32(header + 13, (unsigned long)length);
	fwrite(header, 1, sizeof(header), file);
	fwrite(cache.shaders[index].source, 1, length, file);
	cache.shader_written[index] = 1;
}

/* a program pair the file has not got, with its shaders' sources if the
file has not got them either */
static void cache_append_program(long index)
{
	FILE *file;
	long vertex = shader_find(cache.programs[index].vertex);
	long fragment = shader_find(cache.programs[index].fragment);
	unsigned char record[17];

	if (cache.program_written[index] || vertex < 0 || fragment < 0)
		return;
	file = fopen(cache.path, "ab");
	if (!file)
		return;
	if (!cache.started)
	{
		fwrite(CACHE_MAGIC, 1, 4, file);
		cache.started = TRUE;
	}
	if (!cache.shader_written[vertex])
		cache_append_shader(file, vertex);
	if (!cache.shader_written[fragment])
		cache_append_shader(file, fragment);
	record[0] = 'P';
	write_u64(record + 1, cache.programs[index].vertex);
	write_u64(record + 9, cache.programs[index].fragment);
	fwrite(record, 1, sizeof(record), file);
	fclose(file);
	cache.program_written[index] = 1;
}

static void cache_load(void)
{
	const char *root;
	long index;

	if (cache.loaded)
		return;
	cache.loaded = TRUE;
	root = platform_data_root();
	snprintf(cache.path, sizeof(cache.path), "%s/%s", root ? root : ".", CACHE_FILE_NAME);
	if (!cache_read())
	{
		platform_log("shader cache: %s did not read; starting it again", cache.path);
		cache_rewrite();
		for (index = 0; index < cache.program_count; index++)
			cache_append_program(index);
	}
}

void xgpu_shader_cache_warm_up(void)
{
	GLuint *names;
	long index, linked = 0;
	unsigned long start = SDL_GetTicks();

	cache_load();
	if (!cache.program_count)
		return;
	names = calloc((size_t)cache.shader_count, sizeof(*names));
	if (!names)
		return;
	/* every shader, then every program, then (only then) their results: the
	browser compiles them all meanwhile, on as many threads as it has */
	for (index = 0; index < cache.shader_count; index++)
	{
		const char *source = cache.shaders[index].source;

		names[index] = glCreateShader(cache.shaders[index].type);
		glShaderSource(names[index], 1, &source, NULL);
		glCompileShader(names[index]);
	}
	for (index = 0; index < cache.program_count; index++)
	{
		long vertex = shader_find(cache.programs[index].vertex);
		long fragment = shader_find(cache.programs[index].fragment);
		GLuint program;

		if (vertex < 0 || fragment < 0)
			continue;
		program = glCreateProgram();
		glAttachShader(program, names[vertex]);
		glAttachShader(program, names[fragment]);
		glLinkProgram(program);
		cache.programs[index].program = program;
	}
	for (index = 0; index < cache.program_count; index++)
	{
		GLint status = 0;
		GLuint program = cache.programs[index].program;

		if (!program)
			continue;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (!status)
		{
			glDeleteProgram(program);
			cache.programs[index].program = 0;
			continue;
		}
		linked++;
	}
	/* (the programs keep what they were linked from) */
	for (index = 0; index < cache.shader_count; index++)
		glDeleteShader(names[index]);
	free(names);
	platform_log("shader cache: %ld of %ld programs warmed up in %lu ms", linked, cache.program_count,
		(unsigned long)(SDL_GetTicks() - start));
}

void xgpu_shader_cache_shader_compiled(GLuint shader, GLenum type, const char *source)
{
	hash64 hash;

	cache_load();
	if (!shader || !source)
		return;
	if (shader >= cache.name_capacity)
	{
		unsigned long wanted = cache.name_capacity ? cache.name_capacity : 1024;
		hash64 *bigger;

		while (wanted <= shader)
			wanted *= 2;
		bigger = realloc(cache.names, wanted * sizeof(*bigger));
		if (!bigger)
			return;
		memset(bigger + cache.name_capacity, 0, (wanted - cache.name_capacity) * sizeof(*bigger));
		cache.names = bigger;
		cache.name_capacity = wanted;
	}
	hash = source_hash(type, source);
	cache.names[shader] = hash;
	shader_add(hash, type, source, strlen(source));
}

static hash64 name_hash(GLuint shader)
{
	return shader < cache.name_capacity ? cache.names[shader] : 0;
}

GLuint xgpu_shader_cache_program(GLuint vertex_shader, GLuint fragment_shader)
{
	hash64 vertex = name_hash(vertex_shader), fragment = name_hash(fragment_shader);
	long index;
	GLuint program;

	if (!vertex || !fragment || (index = program_find(vertex, fragment)) < 0 || !cache.programs[index].program)
		return 0;
	program = cache.programs[index].program;
	cache.programs[index].program = 0;
	return program;
}

void xgpu_shader_cache_program_linked(GLuint vertex_shader, GLuint fragment_shader)
{
	hash64 vertex = name_hash(vertex_shader), fragment = name_hash(fragment_shader);
	long index;

	if (!vertex || !fragment)
		return;
	index = program_add(vertex, fragment);
	if (index >= 0)
		cache_append_program(index);
}

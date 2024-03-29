#include "mupdf/fitz.h"

fz_pixmap *
fz_new_pixmap_from_image(fz_context *ctx, fz_image *image, int w, int h)
{
	if (image == NULL)
		return NULL;
	return image->get_pixmap(ctx, image, w, h);
}

fz_image *
fz_keep_image(fz_context *ctx, fz_image *image)
{
	return (fz_image *)fz_keep_storable(ctx, &image->storable);
}

void
fz_drop_image(fz_context *ctx, fz_image *image)
{
	fz_drop_storable(ctx, &image->storable);
}

typedef struct fz_image_key_s fz_image_key;

struct fz_image_key_s {
	int refs;
	fz_image *image;
	int l2factor;
};

static int
fz_make_hash_image_key(fz_store_hash *hash, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;

	hash->u.pi.ptr = key->image;
	hash->u.pi.i = key->l2factor;
	return 1;
}

static void *
fz_keep_image_key(fz_context *ctx, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;

	fz_lock(ctx, FZ_LOCK_ALLOC);
	key->refs++;
	fz_unlock(ctx, FZ_LOCK_ALLOC);

	return (void *)key;
}

static void
fz_drop_image_key(fz_context *ctx, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;
	int drop;

	if (key == NULL)
		return;
	fz_lock(ctx, FZ_LOCK_ALLOC);
	drop = --key->refs;
	fz_unlock(ctx, FZ_LOCK_ALLOC);
	if (drop == 0)
	{
		fz_drop_image(ctx, key->image);
		fz_free(ctx, key);
	}
}

static int
fz_cmp_image_key(void *k0_, void *k1_)
{
	fz_image_key *k0 = (fz_image_key *)k0_;
	fz_image_key *k1 = (fz_image_key *)k1_;

	return k0->image == k1->image && k0->l2factor == k1->l2factor;
}

#ifndef NDEBUG
static void
fz_debug_image(FILE *out, void *key_)
{
	fz_image_key *key = (fz_image_key *)key_;

	fprintf(out, "(image %d x %d sf=%d) ", key->image->w, key->image->h, key->l2factor);
}
#endif

static fz_store_type fz_image_store_type =
{
	fz_make_hash_image_key,
	fz_keep_image_key,
	fz_drop_image_key,
	fz_cmp_image_key,
#ifndef NDEBUG
	fz_debug_image
#endif
};

static void
fz_mask_color_key(fz_pixmap *pix, int n, int *colorkey)
{
	unsigned char *p = pix->samples;
	int len = pix->w * pix->h;
	int k, t;
	while (len--)
	{
		t = 1;
		for (k = 0; k < n; k++)
			if (p[k] < colorkey[k * 2] || p[k] > colorkey[k * 2 + 1])
				t = 0;
		if (t)
			for (k = 0; k < pix->n; k++)
				p[k] = 0;
		p += pix->n;
	}
}

fz_pixmap *
fz_decomp_image_from_stream(fz_context *ctx, fz_stream *stm, fz_image *image, int in_line, int indexed, int l2factor, int native_l2factor)
{
	fz_pixmap *tile = NULL;
	int stride, len, i;
	unsigned char *samples = NULL;
	int f = 1<<native_l2factor;
	int w = (image->w + f-1) >> native_l2factor;
	int h = (image->h + f-1) >> native_l2factor;

	fz_var(tile);
	fz_var(samples);

	fz_try(ctx)
	{
		tile = fz_new_pixmap(ctx, image->colorspace, w, h);
		tile->interpolate = image->interpolate;

		stride = (w * image->n * image->bpc + 7) / 8;

		samples = fz_malloc_array(ctx, h, stride);

		len = fz_read(stm, samples, h * stride);

		/* Make sure we read the EOF marker (for inline images only) */
		if (in_line)
		{
			unsigned char tbuf[512];
			fz_try(ctx)
			{
				int tlen = fz_read(stm, tbuf, sizeof tbuf);
				if (tlen > 0)
					fz_warn(ctx, "ignoring garbage at end of image");
			}
			fz_catch(ctx)
			{
				fz_rethrow_if(ctx, FZ_ERROR_TRYLATER);
				fz_warn(ctx, "ignoring error at end of image");
			}
		}

		/* Pad truncated images */
		if (len < stride * h)
		{
			fz_warn(ctx, "padding truncated image");
			memset(samples + len, 0, stride * h - len);
		}

		/* Invert 1-bit image masks */
		if (image->imagemask)
		{
			/* 0=opaque and 1=transparent so we need to invert */
			unsigned char *p = samples;
			len = h * stride;
			for (i = 0; i < len; i++)
				p[i] = ~p[i];
		}

		fz_unpack_tile(tile, samples, image->n, image->bpc, stride, indexed);

		fz_free(ctx, samples);
		samples = NULL;

		if (image->usecolorkey)
			fz_mask_color_key(tile, image->n, image->colorkey);

		if (indexed)
		{
			fz_pixmap *conv;
			fz_decode_indexed_tile(tile, image->decode, (1 << image->bpc) - 1);
			conv = fz_expand_indexed_pixmap(ctx, tile);
			fz_drop_pixmap(ctx, tile);
			tile = conv;
		}
		else
		{
			fz_decode_tile(tile, image->decode);
		}
	}
	fz_always(ctx)
	{
		fz_close(stm);
	}
	fz_catch(ctx)
	{
		if (tile)
			fz_drop_pixmap(ctx, tile);
		fz_free(ctx, samples);

		fz_rethrow(ctx);
	}

	/* Now apply any extra subsampling required */
	if (l2factor - native_l2factor > 0)
	{
		if (l2factor - native_l2factor > 8)
			l2factor = native_l2factor + 8;
		fz_subsample_pixmap(ctx, tile, l2factor - native_l2factor);
	}

	return tile;
}

void
fz_free_image(fz_context *ctx, fz_storable *image_)
{
	fz_image *image = (fz_image *)image_;

	if (image == NULL)
		return;
	fz_drop_pixmap(ctx, image->tile);
	fz_free_compressed_buffer(ctx, image->buffer);
	fz_drop_colorspace(ctx, image->colorspace);
	fz_drop_image(ctx, image->mask);
	fz_free(ctx, image);
}

fz_pixmap *
fz_image_get_pixmap(fz_context *ctx, fz_image *image, int w, int h)
{
	fz_pixmap *tile;
	fz_stream *stm;
	int l2factor;
	fz_image_key key;
	int native_l2factor;
	int indexed;
	fz_image_key *keyp;

	/* Check for 'simple' images which are just pixmaps */
	if (image->buffer == NULL)
	{
		tile = image->tile;
		if (!tile)
			return NULL;
		return fz_keep_pixmap(ctx, tile); /* That's all we can give you! */
	}

	/* Ensure our expectations for tile size are reasonable */
	if (w > image->w)
		w = image->w;
	if (h > image->h)
		h = image->h;

	/* What is our ideal factor? */
	if (w == 0 || h == 0)
		l2factor = 0;
	else
		for (l2factor=0; image->w>>(l2factor+1) >= w && image->h>>(l2factor+1) >= h && l2factor < 8; l2factor++);

	/* Can we find any suitable tiles in the cache? */
	key.refs = 1;
	key.image = image;
	key.l2factor = l2factor;
	do
	{
		tile = fz_find_item(ctx, fz_free_pixmap_imp, &key, &fz_image_store_type);
		if (tile)
			return tile;
		key.l2factor--;
	}
	while (key.l2factor >= 0);

	/* We need to make a new one. */
	/* First check for ones that we can't decode using streams */
	switch (image->buffer->params.type)
	{
	case FZ_IMAGE_PNG:
		tile = fz_load_png(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_TIFF:
		tile = fz_load_tiff(ctx, image->buffer->buffer->data, image->buffer->buffer->len);
		break;
	case FZ_IMAGE_JXR:
		fz_throw(ctx, FZ_ERROR_GENERIC, "JPEG-XR codec is not available");
		break;
	default:
		native_l2factor = l2factor;
		stm = fz_open_image_decomp_stream(ctx, image->buffer, &native_l2factor);

		indexed = fz_colorspace_is_indexed(image->colorspace);
		tile = fz_decomp_image_from_stream(ctx, stm, image, 0, indexed, l2factor, native_l2factor);

		/* CMYK JPEGs in XPS documents have to be inverted */
		if (image->invert_cmyk_jpeg &&
			image->buffer->params.type == FZ_IMAGE_JPEG &&
			image->colorspace == fz_device_cmyk(ctx) &&
			image->buffer->params.u.jpeg.color_transform)
		{
			fz_invert_pixmap(ctx, tile);
		}

		break;
	}

	/* Now we try to cache the pixmap. Any failure here will just result
	 * in us not caching. */
	fz_var(keyp);
	fz_try(ctx)
	{
		fz_pixmap *existing_tile;

		keyp = fz_malloc_struct(ctx, fz_image_key);
		keyp->refs = 1;
		keyp->image = fz_keep_image(ctx, image);
		keyp->l2factor = l2factor;
		existing_tile = fz_store_item(ctx, keyp, tile, fz_pixmap_size(ctx, tile), &fz_image_store_type);
		if (existing_tile)
		{
			/* We already have a tile. This must have been produced by a
			 * racing thread. We'll throw away ours and use that one. */
			fz_drop_pixmap(ctx, tile);
			tile = existing_tile;
		}
	}
	fz_always(ctx)
	{
		fz_drop_image_key(ctx, keyp);
	}
	fz_catch(ctx)
	{
		/* Do nothing */
	}

	return tile;
}

fz_image *
fz_new_image_from_pixmap(fz_context *ctx, fz_pixmap *pixmap, fz_image *mask)
{
	fz_image *image;

	assert(mask == NULL || mask->mask == NULL);

	fz_try(ctx)
	{
		image = fz_malloc_struct(ctx, fz_image);
		FZ_INIT_STORABLE(image, 1, fz_free_image);
		image->w = pixmap->w;
		image->h = pixmap->h;
		image->n = pixmap->n;
		image->colorspace = pixmap->colorspace;
		image->bpc = 8;
		image->buffer = NULL;
		image->get_pixmap = fz_image_get_pixmap;
		image->xres = pixmap->xres;
		image->yres = pixmap->yres;
		image->tile = pixmap;
		image->mask = mask;
	}
	fz_catch(ctx)
	{
		fz_drop_image(ctx, mask);
		fz_rethrow(ctx);
	}
	return image;
}

fz_image *
fz_new_image(fz_context *ctx, int w, int h, int bpc, fz_colorspace *colorspace,
	int xres, int yres, int interpolate, int imagemask, float *decode,
	int *colorkey, fz_compressed_buffer *buffer, fz_image *mask)
{
	fz_image *image;

	assert(mask == NULL || mask->mask == NULL);

	fz_try(ctx)
	{
		image = fz_malloc_struct(ctx, fz_image);
		FZ_INIT_STORABLE(image, 1, fz_free_image);
		image->get_pixmap = fz_image_get_pixmap;
		image->w = w;
		image->h = h;
		image->xres = xres;
		image->yres = yres;
		image->bpc = bpc;
		image->n = (colorspace ? colorspace->n : 1);
		image->colorspace = colorspace;
		image->interpolate = interpolate;
		image->imagemask = imagemask;
		image->usecolorkey = (colorkey != NULL);
		if (colorkey)
			memcpy(image->colorkey, colorkey, sizeof(int)*image->n*2);
		if (decode)
			memcpy(image->decode, decode, sizeof(float)*image->n*2);
		else
		{
			float maxval = fz_colorspace_is_indexed(colorspace) ? (1 << bpc) - 1 : 1;
			int i;
			for (i = 0; i < image->n; i++)
			{
				image->decode[2*i] = 0;
				image->decode[2*i+1] = maxval;
			}
		}
		image->mask = mask;
		image->buffer = buffer;
	}
	fz_catch(ctx)
	{
		fz_free_compressed_buffer(ctx, buffer);
		fz_rethrow(ctx);
	}

	return image;
}

fz_image *
fz_new_image_from_data(fz_context *ctx, unsigned char *data, int len)
{
	fz_buffer *buffer = NULL;
	fz_image *image;

	fz_var(buffer);
	fz_var(data);

	fz_try(ctx)
	{
		buffer = fz_new_buffer_from_data(ctx, data, len);
		data = NULL;
		image = fz_new_image_from_buffer(ctx, buffer);
	}
	fz_always(ctx)
	{
		fz_drop_buffer(ctx, buffer);
	}
	fz_catch(ctx)
	{
		fz_free(ctx, data);
		fz_rethrow(ctx);
	}

	return image;
}

fz_image *
fz_new_image_from_buffer(fz_context *ctx, fz_buffer *buffer)
{
	fz_compressed_buffer *bc = NULL;
	int w, h, xres, yres;
	fz_colorspace *cspace;
	int len = buffer->len;
	unsigned char *buf = buffer->data;

	fz_var(bc);

	fz_try(ctx)
	{
		if (len < 8)
			fz_throw(ctx, FZ_ERROR_GENERIC, "unknown image file format");

		bc = fz_malloc_struct(ctx, fz_compressed_buffer);
		bc->buffer = fz_keep_buffer(ctx, buffer);

		if (buf[0] == 0xff && buf[1] == 0xd8)
		{
			bc->params.type = FZ_IMAGE_JPEG;
			bc->params.u.jpeg.color_transform = -1;
			fz_load_jpeg_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (memcmp(buf, "\211PNG\r\n\032\n", 8) == 0)
		{
			bc->params.type = FZ_IMAGE_PNG;
			fz_load_png_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else if (memcmp(buf, "II", 2) == 0 && buf[2] == 0xBC)
		{
			bc->params.type = FZ_IMAGE_JXR;
			fz_throw(ctx, FZ_ERROR_GENERIC, "JPEG-XR codec is not available");
		}
		else if (memcmp(buf, "MM", 2) == 0 || memcmp(buf, "II", 2) == 0)
		{
			bc->params.type = FZ_IMAGE_TIFF;
			fz_load_tiff_info(ctx, buf, len, &w, &h, &xres, &yres, &cspace);
		}
		else
			fz_throw(ctx, FZ_ERROR_GENERIC, "unknown image file format");
	}
	fz_catch(ctx)
	{
		fz_free_compressed_buffer(ctx, bc);
		fz_rethrow(ctx);
	}

	return fz_new_image(ctx, w, h, 8, cspace, xres, yres, 0, 0, NULL, NULL, bc, NULL);
}

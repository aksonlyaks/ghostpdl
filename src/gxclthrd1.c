/* Copyright (C) 2001-2006 Artifex Software, Inc.
   All Rights Reserved.
  
   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied, modified
   or distributed except as expressly authorized under the terms of that
   license.  Refer to licensing information at http://www.artifex.com/
   or contact Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134,
   San Rafael, CA  94903, U.S.A., +1(415)492-9861, for further information.
*/

/*$Id$ */
/* Command list - Support for multiple rendering threads */
#include "memory_.h"
#include "gx.h"
#include "gpcheck.h"
#include "gxsync.h"
#include "gserrors.h"
#include "gxdevice.h"
#include "gsdevice.h"
#include "gscoord.h"		/* requires gsmatrix.h */
#include "gxdevmem.h"		/* must precede gxcldev.h */
#include "gdevprn.h"		/* must precede gxcldev.h */
#include "gxcldev.h"
#include "gxgetbit.h"
#include "gdevplnx.h"
#include "gsmemory.h"
#include "gxclthrd.h"

/* Forward reference prototypes */
static int clist_start_render_thread(gx_device *dev, int thread_index, int band);
static void clist_render_thread(void *param);


/* Set up and start the render threads */
static int
clist_setup_render_threads(gx_device *dev, int y)
{
    gx_device_printer *pdev = (gx_device_printer *)dev;
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_common *cdev = (gx_device_clist_common *)cldev;
    gx_device_clist_reader *crdev = &cldev->reader;
    gs_memory_t *mem = cdev->bandlist_memory;
    gx_device *protodev;
    gs_c_param_list paramlist;
    int i, code, band;
    int band_count = cdev->nbands;
    char fmode[4];

    crdev->num_render_threads = pdev->num_render_threads_requested;

    if(gs_debug[':'] != 0)
	dprintf1("Attempting to set up %d rendering threads\n", pdev->num_render_threads_requested);

    if (crdev->num_render_threads > band_count)
	crdev->num_render_threads = band_count;	/* don't bother starting more threads than bands */

    /* Allocate and initialize an array of thread control structures */
    crdev->render_threads = (clist_render_thread_control_t *)
	      gs_alloc_byte_array(mem, crdev->num_render_threads,
	      sizeof(clist_render_thread_control_t), "clist_setup_render_threads" );
    /* fallback to non-threaded if allocation fails */
    if (crdev->render_threads == NULL)
	return_error(gs_error_VMerror);

    memset(crdev->render_threads, 0, crdev->num_render_threads *
	    sizeof(clist_render_thread_control_t));
    crdev->main_thread_data = cdev->data;		/* save data area */
    /* Based on the line number requested, decide the order of band rendering */
    if (y == 0) {
	crdev->thread_lookahead_direction = 1;
	band = 0;
    } else {
	crdev->thread_lookahead_direction = -1;
	band = band_count;
    }

    /* Close the files so we can open them in multiple threads */
    /* TODO: This doesn't work for memfile clist yet, so will fail */
    if ((code = cdev->page_info.io_procs->fclose(cdev->page_cfile, cdev->page_cfname, false)) < 0 ||
        (code = cdev->page_info.io_procs->fclose(cdev->page_bfile, cdev->page_bfname, false)) < 0) {
	gs_free_object(mem, crdev->render_threads, "clist_setup_render_threads");
	crdev->render_threads = NULL;
        return_error(gs_error_unknownerror); /* shouldn't happen */
    }
    cdev->page_cfile = cdev->page_bfile = NULL;
    strcpy(fmode, "r");			/* read access for threads */
    strcat(fmode, gp_fmode_binary_suffix);
    /* Find the prototype for this device (needed so we can copy from it) */
    for (i=0; (protodev = (gx_device *)gs_getdevice(i)) != NULL; i++)
	if (strcmp(protodev->dname, dev->dname) == 0)
	    break;
    if (protodev == NULL)
	return gs_error_rangecheck;

    gs_c_param_list_write(&paramlist, mem);
    if ((code = gs_getdeviceparams(dev, (gs_param_list *)&paramlist)) < 0)
	return code;

    /* Loop creating the devices and semaphores for each thread, then start them */
    for (i=0; i < crdev->num_render_threads; i++, band += crdev->thread_lookahead_direction) {
	gx_device *ndev;
	gx_device_clist *ncldev;
	gx_device_clist_common *ncdev;
	clist_render_thread_control_t *thread = &(crdev->render_threads[i]);

        thread->band = -1;		/* a value that won't match any valid band */
	if ((code = gs_copydevice((gx_device **) &ndev, protodev, mem)) < 0) {
	    code = 0;		/* even though we failed, no cleanup needed */
	    break;
	}
	ncldev = (gx_device_clist *)ndev;
	ncdev = (gx_device_clist_common *)ndev;
	gx_device_fill_in_procs(ndev);
	((gx_device_printer *)ncdev)->buffer_memory = ncdev->memory =
		ncdev->bandlist_memory = mem;
	gs_c_param_list_read(&paramlist);
	ndev->PageCount = dev->PageCount;	/* copy to prevent mismatch error */
	if ((code = gs_putdeviceparams(ndev, (gs_param_list *)&paramlist)) < 0)
	    break;
	ncdev->page_uses_transparency = cdev->page_uses_transparency;
	/* gdev_prn_allocate_memory sets the clist for writing, creating new files.
	 * We need  to unlink those files and open the main thread's files, then
	 * reset the clist state for reading/rendering
	 */
	if ((code = gdev_prn_allocate_memory(ndev, NULL, 0, 0)) < 0)
	    break;
	thread->cdev = ndev;
	/* close and unlink the temp files just created */
	cdev->page_info.io_procs->fclose(ncdev->page_cfile, ncdev->page_cfname, true);
	cdev->page_info.io_procs->fclose(ncdev->page_bfile, ncdev->page_bfname, true);
	/* open the main thread's files for this thread */
	if ((code=cdev->page_info.io_procs->fopen(cdev->page_cfname, fmode, &ncdev->page_cfile,
			    mem, mem, true)) < 0 ||
	     (code=cdev->page_info.io_procs->fopen(cdev->page_bfname, fmode, &ncdev->page_bfile,
			    mem, mem, false)) < 0)
	    break;
	clist_render_init(ncldev);	/* Initialize clist device for reading */
	ncdev->page_bfile_end_pos = cdev->page_bfile_end_pos;

	/* create the buf device for this thread, and allocate the semaphores */
	if ((code = gdev_create_buf_device(cdev->buf_procs.create_buf_device,
				&(thread->bdev), cdev->target,
				band*crdev->page_band_height, NULL,
				mem, clist_get_band_complexity(dev,y)) < 0)) 
	    break;
	if ((thread->sema_this = gx_semaphore_alloc(mem)) == NULL ||
	    (thread->sema_group = gx_semaphore_alloc(mem)) == NULL) {
	    code = gs_error_VMerror;
	    break;
	}
	/* Start thread 'i' to do band */
	if ((code = clist_start_render_thread(dev, i, band)) < 0)
	    break;
    }
    gs_c_param_list_release(&paramlist);
    /* If the code < 0, the last thread creation failed -- clean it up */
    if (code < 0) {
	/* the following relies on 'free' ignoring NULL pointers */
	gx_semaphore_free(crdev->render_threads[i].sema_group); 
	gx_semaphore_free(crdev->render_threads[i].sema_this); 
	if (crdev->render_threads[i].bdev != NULL)
	    cdev->buf_procs.destroy_buf_device(crdev->render_threads[i].bdev);
	if (crdev->render_threads[i].cdev != NULL) {
	    gdev_prn_free_memory((gx_device *)(crdev->render_threads[i].cdev));
	    gs_free_object(mem, crdev->render_threads[i].cdev, "clist_setup_render_threads");
	}
    }
    /* If we weren't able to create at least one thread, punt	*/
    /* Although a single thread isn't any more efficient, the	*/
    /* machinery still works, so that's OK.			*/
    if (i == 0) {
	gs_free_object(mem, crdev->render_threads, "clist_setup_render_threads");
	crdev->render_threads = NULL;
	pdev->num_render_threads_requested = 0;	/* shut down thread support */
	/* restore the file pointers */
	if (cdev->page_cfile == NULL) {
	    char fmode[4];

	    strcpy(fmode, "w+");
	    strcat(fmode, gp_fmode_binary_suffix);
	    cdev->page_info.io_procs->fopen(cdev->page_cfname, fmode, &cdev->page_cfile,
				mem, cdev->bandlist_memory, true);
	    cdev->page_info.io_procs->fopen(cdev->page_bfname, fmode, &cdev->page_bfile,
				mem, cdev->bandlist_memory, false);
	}
	return_error(code);
    }
    crdev->num_render_threads = i;
    crdev->curr_render_thread = 0;

    if(gs_debug[':'] != 0)
	dprintf1("Using %d rendering threads\n", i);

    return 0;
}

void
clist_teardown_render_threads(gx_device *dev)
{
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_common *cdev = (gx_device_clist_common *)dev;
    gx_device_clist_reader *crdev = &cldev->reader;
    gs_memory_t *mem = cdev->bandlist_memory;
    int i;

    if (crdev->render_threads != NULL) {

	/* Wait for each thread to finish then free its memory */
	for (i=0; i < crdev->num_render_threads; i++) {
	    clist_render_thread_control_t *thread = &(crdev->render_threads[i]);
	    gx_device_clist_common *thread_cdev = (gx_device_clist_common *)thread->cdev;

	    if (thread->status == RENDER_THREAD_BUSY)
		gx_semaphore_wait(thread->sema_this);
	    /* Free control semaphores */
	    gx_semaphore_free(thread->sema_group);
	    gx_semaphore_free(thread->sema_this);
	    /* destroy the thread's buffer device */
	    cdev->buf_procs.destroy_buf_device(thread->bdev);
	    /*
	     * Free the BufferSpace, close the band files 
	     * Note that the BufferSpace is freed using 'ppdev->buf' so the 'data'
	     * pointer doesn't need to be the one that the thread started with
	     */
	    gdev_prn_free_memory(thread->cdev);
	    /* Free the device copy this thread used */
	    gs_free_object(mem, thread->cdev, "clist_teardown_render_threads");
	}
	cdev->data = crdev->main_thread_data;	/* restore the pointer for writing */
	gs_free_object(mem, crdev->render_threads, "clist_teardown_render_threads");
	crdev->render_threads = NULL;

	/* Now re-open the clist temp files so we can write to them */
	if (cdev->page_cfile == NULL) {
	    char fmode[4];

	    strcpy(fmode, "w+");
	    strcat(fmode, gp_fmode_binary_suffix);
	    cdev->page_info.io_procs->fopen(cdev->page_cfname, fmode, &cdev->page_cfile,
				mem, cdev->bandlist_memory, true);
	    cdev->page_info.io_procs->fopen(cdev->page_bfname, fmode, &cdev->page_bfile,
				mem, cdev->bandlist_memory, false);
	}
    }
}

static int
clist_start_render_thread(gx_device *dev, int thread_index, int band)
{
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_reader *crdev = &cldev->reader;
    int code;

    crdev->render_threads[thread_index].band = band;
    crdev->render_threads[thread_index].status = RENDER_THREAD_BUSY;
    
    /* Finally, fire it up */
    code = gp_create_thread(clist_render_thread, &(crdev->render_threads[thread_index]));

    return code;
}

static void
clist_render_thread(void *data)
{
    clist_render_thread_control_t *thread = (clist_render_thread_control_t *)data;
    gx_device *dev = thread->cdev;
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_reader *crdev = &cldev->reader;
    gx_device *bdev = thread->bdev;
    gs_int_rect band_rect;
    byte *mdata = crdev->data + crdev->page_tile_cache_size;
    uint raster = bitmap_raster(dev->width * dev->color_info.depth);
    int code;
    int band_height = crdev->page_band_height;
    int band = thread->band;
    int band_begin_line = band * band_height;
    int band_end_line = band_begin_line + band_height;
    int band_num_lines;

    if (band_end_line > dev->height)
	band_end_line = dev->height;
    band_num_lines = band_end_line - band_begin_line;

    code = crdev->buf_procs.setup_buf_device
	    (bdev, mdata, raster, NULL, 0, band_num_lines, band_num_lines);
    band_rect.p.x = 0;
    band_rect.p.y = band_begin_line;
    band_rect.q.x = dev->width;
    band_rect.q.y = band_end_line;
    if (code >= 0)
	code = clist_render_rectangle(cldev, &band_rect, bdev, NULL, true);
    /* Reset the band boundaries now */
    crdev->ymin = band_begin_line;
    crdev->ymax = band_end_line;
    crdev->offset_map = NULL;
    if (code < 0)
	thread->status = code;		/* shouldn't happen */
    else
	thread->status = RENDER_THREAD_DONE;	/* OK */

    /*
     * Signal the semaphores. We signal the 'group' first since even if
     * the waiter is released on the group, it still needs to check
     * status on the thread
     */
    gx_semaphore_signal(thread->sema_group);
    gx_semaphore_signal(thread->sema_this);
}

/*
 * Copy the raster data from the completed thread to the caller's
 * device (the main thread)
 * Return 0 if OK, < 0 is the error code from the thread 
 *
 * After swapping the pointers, start up the completed thread with the
 * next band remaining to do (if any)
 */
static int
clist_get_band_from_thread(gx_device *dev, int band)
{
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_common *cdev = (gx_device_clist_common *)dev;
    gx_device_clist_reader *crdev = &cldev->reader;
    int next_band, code = 0;
    int thread_index = crdev->curr_render_thread;
    clist_render_thread_control_t *thread = &(crdev->render_threads[thread_index]);
    gx_device_clist_common *thread_cdev = (gx_device_clist_common *)thread->cdev;
    int band_height = crdev->page_info.band_params.BandHeight;
    int band_count = cdev->nbands;
    byte *tmp;			/* for swapping data areas */

    /* We expect that the thread needed will be the 'current' thread */
    if (thread->band != band) {
	/*
	 *TODO: maybe we should search for it, and if not found wait for
	 * and idle thread and start that one
	 */
	eprintf2("clist_get_band_from_thread: at band %d, needed band %d\n",
		thread->band, band);
        return_error(gs_error_rangecheck);
    }
    /* Wait for this thread */
    gx_semaphore_wait(thread->sema_this);
    if (thread->status < 0)
	return thread->status;		/* FAIL */

    /* Swap the data areas to avoid the copy */
    tmp = cdev->data;
    cdev->data = thread_cdev->data;
    thread_cdev->data = tmp;
    thread->status = RENDER_THREAD_IDLE;	/* the data is no longer valid */
    thread->band = -1;
    /* Update the bounds for this band */
    cdev->ymin =  band * band_height;
    cdev->ymax =  cdev->ymin + band_height;
    if (cdev->ymax > dev->height)
	cdev->ymax = dev->height;

    /* If we are not at the final band, start up this thread with the next one to do */
    next_band = band + (crdev->num_render_threads * crdev->thread_lookahead_direction);
    if (next_band > 0 && next_band < band_count)
	code = clist_start_render_thread(dev, thread_index, next_band);
    /* bump the 'curr' to the next thread */
    crdev->curr_render_thread = crdev->curr_render_thread == crdev->num_render_threads - 1 ?
		0 : crdev->curr_render_thread + 1;

    return code;
}

/* Copy a rasterized rectangle to the client, rasterizing if needed. */
/* The first invocation starts multiple threads to perform "look ahead" */
/* rendering adjacent to the first band (forward or backward) */
static int
clist_get_bits_rect_mt(gx_device *dev, const gs_int_rect * prect,
			 gs_get_bits_params_t *params, gs_int_rect **unread)
{
    gx_device_printer *pdev = (gx_device_printer *)dev;
    gx_device_clist *cldev = (gx_device_clist *)dev;
    gx_device_clist_common *cdev = (gx_device_clist_common *)dev;
    gx_device_clist_reader *crdev = &cldev->reader;
    gs_memory_t *mem = cdev->bandlist_memory;
    gs_get_bits_options_t options = params->options;
    int y = prect->p.y;
    int end_y = prect->q.y;
    int line_count = end_y - y;
    int band_height = crdev->page_info.band_params.BandHeight;
    int band = y / band_height;
    gs_int_rect band_rect;
    int lines_rasterized;
    gx_device *bdev;
    byte *mdata;
    uint raster = bitmap_raster(dev->width * dev->color_info.depth);
    int my;
    int code = 0;

    /* This page might not want multiple threads */
    /* Also we don't support plane extraction using multiple threads */
    if (pdev->num_render_threads_requested < 1 || (options & GB_SELECT_PLANES))
	return clist_get_bits_rectangle(dev, prect, params, unread);

    if (prect->p.x < 0 || prect->q.x > dev->width ||
	y < 0 || end_y > dev->height
	)
	return_error(gs_error_rangecheck);
    if (line_count <= 0 || prect->p.x >= prect->q.x)
	return 0;

    if((code = clist_close_writer_and_init_reader(cldev)) < 0)
	return code;
    
    if (crdev->render_threads == NULL) {
        if ((code = clist_setup_render_threads(dev, y)) < 0) {
	    /* revert to the default single threaded rendering */
	    return clist_get_bits_rectangle(dev, prect, params, unread);
	}
    } 
    /* If we already have the band's data, just return it */
    if (y < crdev->ymin || end_y > crdev->ymax)
	code = clist_get_band_from_thread(dev, band);
    if (code < 0)
	goto free_thread_out;
    mdata = crdev->data + crdev->page_tile_cache_size;
    if ((code = gdev_create_buf_device(cdev->buf_procs.create_buf_device,
				  &bdev, cdev->target, y, NULL,
				  mem, clist_get_band_complexity(dev,y))) < 0 ||
	(code = crdev->buf_procs.setup_buf_device(bdev, mdata, raster, NULL,
			    y - crdev->ymin, line_count, crdev->ymax - crdev->ymin)) < 0)
	goto free_thread_out;

    lines_rasterized = min(band_height, line_count);
    /* Return as much of the rectangle as falls within the rasterized lines. */
    band_rect = *prect;
    band_rect.p.y = 0;
    band_rect.q.y = lines_rasterized;
    code = dev_proc(bdev, get_bits_rectangle)
	(bdev, &band_rect, params, unread);
    cdev->buf_procs.destroy_buf_device(bdev);
    if (code < 0)
	goto free_thread_out;

    /* Note that if called via 'get_bits', the line count will always be 1 */
    if (lines_rasterized == line_count) {
	return code;		
    }

/***** TODO: Handle the below with data from the threads *****/
    /*
     * We'll have to return the rectangle in pieces.  Force GB_RETURN_COPY
     * rather than GB_RETURN_POINTER, and require all subsequent pieces to
     * use the same values as the first piece for all of the other format
     * options.  If copying isn't allowed, or if there are any unread
     * rectangles, punt.
     */
    if (!(options & GB_RETURN_COPY) || code > 0)
	return gx_default_get_bits_rectangle(dev, prect, params, unread);
    options = params->options;
    if (!(options & GB_RETURN_COPY)) {
	/* Redo the first piece with copying. */
	params->options = options =
	    (params->options & ~GB_RETURN_ALL) | GB_RETURN_COPY;
	lines_rasterized = 0;
    }
    {
	gs_get_bits_params_t band_params;
	uint raster = gx_device_raster(bdev, true);

	code = gdev_create_buf_device(cdev->buf_procs.create_buf_device,
				      &bdev, cdev->target, y, NULL,
				      mem, clist_get_band_complexity(dev, y));
	if (code < 0)
	    return code;
	band_params = *params;
	while ((y += lines_rasterized) < end_y) {
	    /* Increment data pointer by lines_rasterized. */
	    if (band_params.data)
		band_params.data[0] += raster * lines_rasterized;
	    line_count = end_y - y;
	    // code = clist_rasterize_lines(dev, y, line_count, bdev, NULL, &my);
	    if (code < 0)
		break;
	    lines_rasterized = min(code, line_count);
	    band_rect.p.y = my;
	    band_rect.q.y = my + lines_rasterized;
	    code = dev_proc(bdev, get_bits_rectangle)
		(bdev, &band_rect, &band_params, unread);
	    if (code < 0)
		break;
	    params->options = options = band_params.options;
	    if (lines_rasterized == line_count)
		break;
	}
	cdev->buf_procs.destroy_buf_device(bdev);
    }
    return code;

/* Free up thread stuff */
free_thread_out:
    clist_teardown_render_threads(dev);
    return code;
}

static void
test_threads(void *dummy)
{
}

int 
clist_enable_multi_thread_render(gx_device *dev)
{   
    gx_device_printer *pdev = (gx_device_printer *)dev;
    int code;

    /* We need to test gp_create_thread since we may be on a platform */
    /* built without working threads, i.e., using gp_nsync.c dummy    */
    /* routines. The nosync gp_create_thread returns a -ve error code */
    if ((code = gp_create_thread(test_threads, NULL)) < 0) {
	if (gs_debug[':'] != 0)
	    dprintf("Using single threaded rendering\n");
	pdev->num_render_threads_requested = 0;
	return code;	/* Threads don't work */
    }

    if (gs_debug[':'] != 0)
	dprintf("Multi threaded rendering enabled.\n");
    set_dev_proc(dev, get_bits_rectangle, clist_get_bits_rect_mt);

    return 1;
}

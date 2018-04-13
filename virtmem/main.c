/*
Main program for the virtual memory project.
Make all of your modifications to this file.
You may add or rearrange any code or data as you need.
The header files page_table.h and disk.h explain
how to use the page table and disk interfaces.
*/

#include "page_table.h"
#include "disk.h"
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct{
	int page;
	int bits;
	int rbit;
} Entry_frame;

int fc = 0, rc = 0, wc = 0;
int npages = 0;
int nframes = 0;
char* rt = NULL;
char* virtmem = NULL;
char* physmem = NULL;
struct disk* disk = NULL;
Entry_frame* f_table = NULL;

int ft_pointer = 0;
int lt_pointer = 0;
int *fifo_arr;

void rm_page(struct page_table *pt, int frame);
void rand_helper(struct page_table *pt, int page);
void fifo_helper(struct page_table *pt, int page);
void lru_helper(struct page_table *pt, int page);

void rm_page(struct page_table *pt, int frame)
{
	//write to disk in the case of dirty bit
	if (f_table[frame].bits & PROT_WRITE){
		disk_write(disk, f_table[frame].page, &physmem[frame*PAGE_SIZE]);
		wc++;
	}
	//set entry and update pointer
	page_table_set_entry(pt, f_table[frame].page, frame, 0);
	f_table[frame].bits = 0;
}

void rand_helper(struct page_table *pt, int page)
{
	int frame_idx = -1;
	int frame;
	int bits;
	page_table_get_entry(pt, page, &frame, &bits);

	if (!bits){
		//set PROT_READ no data in frame;
		bits = PROT_READ;
		//check free frame
		for(int i = 0; i < nframes; i++){
			if (f_table[i].bits == 0) {
				frame_idx = i;
			}
		}
		if (frame_idx < 0){
			frame_idx = lrand48()%nframes;
			rm_page(pt, frame_idx);
		}
		disk_read(disk, page, &physmem[frame_idx * PAGE_SIZE]);
		rc++;
	}
	//make dirty bit and update frame
	else if (bits & PROT_READ){
		bits = PROT_READ | PROT_WRITE;
		frame_idx = frame;
	}
	else{
		printf("Error on rand_helper. \n");
		exit(1);
	}
	//set entry; add data
	page_table_set_entry(pt, page, frame_idx, bits);
	f_table[frame_idx].page = page;
	f_table[frame_idx].bits = bits;
}

void fifo_helper(struct page_table *pt, int page)
{
	int frame_idx = -1;
	int frame;
	int bits;
	page_table_get_entry(pt, page, &frame, &bits);

	if(!bits) {
		//set PROT_READ no data in frame;
		bits = PROT_READ;
		//check free frame
		for(int i = 0; i < nframes; i++){
			if (f_table[i].bits == 0) {
				frame_idx = i;
			}
		}
		if(frame_idx < 0) {
			frame_idx = fifo_arr[ft_pointer];
			rm_page(pt, frame_idx);
			ft_pointer = (ft_pointer + 1) % nframes;
		}
		disk_read(disk, page, &physmem[frame_idx * PAGE_SIZE]);
		rc++;

		fifo_arr[lt_pointer] = frame_idx;
		lt_pointer =(lt_pointer + 1) % nframes;
	}
	else if(bits & PROT_READ){
		bits = PROT_READ | PROT_WRITE;
		frame_idx = frame;
	}
	else {
		printf("Error on fifo\n");
		exit(1);
	}
	//set entry; add data
	page_table_set_entry(pt, page, frame_idx, bits);
	f_table[frame_idx].page = page;
	f_table[frame_idx].bits = bits;
}

void lru_helper(struct page_table *pt, int page)
{
	int frame_idx = -1;
	int frame;
	int bits;
	page_table_get_entry(pt, page, &frame, &bits);

	if(!bits) {
		//set PROT_READ no data in frame;
		bits = PROT_READ;
		//check free frame
		for(int i = 0; i < nframes; i++){
			if (f_table[i].bits == 0) {
				frame_idx = i;
			}
		}
		if(frame_idx < 0){
			for (int i = 0; i < nframes; i++){
				if (f_table[i].rbit == 0) {
					frame_idx = i;
				}
				else {
					f_table[i].rbit = 0;
				}
			}
			frame_idx = fifo_arr[ft_pointer];
			rm_page(pt, frame_idx);
			ft_pointer = (ft_pointer+1)%nframes;
		}
		disk_read(disk, page, &physmem[frame_idx*PAGE_SIZE]);
		rc++;

		//update lst pointer of arr
		fifo_arr[lt_pointer] = frame_idx;
		lt_pointer = (lt_pointer + 1) % nframes;
	}
	else if(bits & PROT_READ){
		bits = PROT_READ | PROT_WRITE;
		frame_idx = frame;
	}
	else {
		printf("Error on lru\n");
		exit(1);
	}
	//set entry; add data
	page_table_set_entry(pt, page, frame_idx, bits);
	f_table[frame_idx].page = page;
	f_table[frame_idx].bits = bits;
	f_table[frame_idx].rbit = 1;
}

void page_fault_handler( struct page_table *pt, int page )
{
	if (!strcmp(rt,"rand")){
		rand_helper(pt, page);
	} else if(!strcmp(rt, "fifo")) {
		fifo_helper(pt, page);
	} else if(!strcmp(rt, "lru")) {
		lru_helper(pt, page);
	}
	fc++;
	// printf("page fault on page #%d\n",page);
	// exit(1);
}

int main( int argc, char *argv[] )
{
	if(argc!=5) {
		printf("use: virtmem <npages> <nframes> <rand|fifo|lru> <sort|scan|focus>\n");
		return 1;
	}

	srand(time(NULL));

	npages = atoi(argv[1]);
	nframes = atoi(argv[2]);
	rt = argv[3];
	const char *program = argv[4];

	disk = disk_open("myvirtualdisk",npages);
	if(!disk) {
		fprintf(stderr,"couldn't create virtual disk: %s\n",strerror(errno));
		return 1;
	}

	struct page_table *pt = page_table_create( npages, nframes, page_fault_handler );
	if(!pt) {
		fprintf(stderr,"couldn't create page table: %s\n",strerror(errno));
		return 1;
	}
	//malloc fifo arr and frame table
	fifo_arr = malloc(nframes * sizeof(int));
	f_table = malloc(nframes * sizeof(Entry_frame));

	if (f_table == NULL){
		printf("Error malloc Entry_frame! \n");
		exit(1);
	}

	virtmem = page_table_get_virtmem(pt);

	physmem = page_table_get_physmem(pt);

	if(!strcmp(program,"sort")) {
		sort_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"scan")) {
		scan_program(virtmem,npages*PAGE_SIZE);

	} else if(!strcmp(program,"focus")) {
		focus_program(virtmem,npages*PAGE_SIZE);

	} else {
		fprintf(stderr,"unknown program: %s\n",argv[4]);

	}

	//free everything you malloced bruhh!!
	free(f_table);
	free(fifo_arr);
	page_table_delete(pt);
	disk_close(disk);

	//
	// printf("Summary\n");
	// printf("Read count: %d\n", rc);
	// printf("Write count: %d\n", wc);
	// printf("Fault count: %d\n", fc);

	return 0;
}

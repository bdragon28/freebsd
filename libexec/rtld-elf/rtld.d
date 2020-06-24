
/*
 * SDT probes used by GDB.
 */
provider rtld {
	probe init_start(int, void*);
	probe init_complete(int, void*);
	probe map_start(int, void*);
	probe map_failed(int, void*);
	probe reloc_complete(int, void*, void*);
	probe unmap_start(int, void*);
	probe unmap_complete(int, void*);
};

/*
	JPKG : Jimmy's Read-Only compressed package format
	Author : Dimitris Vlachos (DimitrisV22@gmail.com @ github.com/DimitrisVlachos)
	LICENCE : MIT
*/

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <vector>
#include <string>
#include <iostream>
#include <stdint.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "file_stream.hpp" 

struct pack_entry_t {
	uint64_t addr;
	uint64_t uncompressed_size;
};

static const std::string cs_signature = "JVFS0100";
static const std::string cs_signature_v1 = "JVFS0101";	/*supports compressed headers*/

static int32_t compress(file_streams::file_stream_if* source,file_streams::file_stream_if* dest,int32_t level);
 

inline void encode(uint64_t data,file_streams::file_stream_if* wr) {
	uint8_t tmp[8];
	tmp[0] = (uint8_t)(data >> (uint64_t)56U) & 0xffU;
	tmp[1] = (uint8_t)(data >> (uint64_t)48U)& 0xffU;
	tmp[2] = (uint8_t)(data >> (uint64_t)40U)& 0xffU;
	tmp[3] = (uint8_t)(data >> (uint64_t)32U)& 0xffU;
	tmp[4] = (uint8_t)(data >> (uint64_t)24U)& 0xffU;
	tmp[5] = (uint8_t)(data >> (uint64_t)16U)& 0xffU;
	tmp[6] = (uint8_t)(data >> (uint64_t)8U)& 0xffU;
	tmp[7] = (uint8_t)(data & (uint64_t)0xffU);

	wr->write(tmp,8);
}

inline void encode(const char* s,file_streams::file_stream_if* wr) {
	wr->write(s,strlen(s));
	wr->write('\0');
}
 
inline void encode(const std::string& s,file_streams::file_stream_if* wr) {
	wr->write(s.c_str(),s.length());
	wr->write('\0');
}

inline bool is_dir(const std::string& path) {
	DIR* dp = opendir(path.c_str());

	if (NULL == dp)
		return false;

	closedir(dp);
	return true;
}

inline uint64_t calc_uncompressed_header_size(const std::vector<std::string>& entries,const std::string& sig = cs_signature) {	
	uint64_t res;

	//(sizeof addr + sizeof uncomp sz + null term str entries[i]) * n_entries
	res = (8U + 8U + 1U) * entries.size();

	//sizeof(sig)+null term + sizeof(hdr_count)
	res += sig.length() + 1U + 8U;

	for (uint32_t i = 0U,j = entries.size();i < j;++i)
		res += entries[i].length();

	return res;
}

bool get_files(const std::string& root,std::vector<std::string>& flist) {
    DIR* dp;
    struct dirent* dirp;
	std::vector<std::string> dirs;

	flist.clear();

	dirs.push_back(root);

	while (!dirs.empty()) {
		std::string path = dirs.back(); 
		dirs.pop_back();

		if (path.empty()) {
			printf("Got empty path ?? \n");
			continue;
		}
		
		if((dp  = opendir(path.c_str())) == NULL) {
		    printf("Unable to open dir %s\n",path.c_str());
		    return false;
		}

		if (path[path.length()-1] != '/')
			path += "/";
 
		while ((dirp = readdir(dp)) != NULL) {
			const char* dn = dirp->d_name;
			const std::string full_path = path + ( (dn) ? std::string(dn) : "" );
			if(!dn)												//Sanity check
				continue;
			else if(dirp->d_name[0] == '.')						//Skip links
				continue;
			else if (is_dir(full_path)) {						//Stack it up
				dirs.push_back(full_path);
				continue;
			}
			flist.push_back(full_path);
			//printf("%s %u\n",full_path.c_str(),is_dir(full_path));
		}
		closedir(dp);
	}
    return true;
}

bool pack(const std::string& package_name,const std::string& root_path,const int32_t compression_level) {
	std::vector<std::string> file_list;
	std::vector<pack_entry_t> entry_list;
	file_streams::file_stream_if* read_stream = 0;
	file_streams::file_stream_if* write_stream = 0;
	uint64_t uncomp_size = 0U;
	pack_entry_t pack_ent;

	printf("Constructing package (compression method : %s)\n",(compression_level == Z_BEST_COMPRESSION) ? "best" : "default");

	get_files(root_path,file_list);
	if (file_list.empty()) {
		printf("Pack : no files in %s\n",root_path.c_str());
		return false;
	}
	entry_list.reserve(file_list.size());

	write_stream = new file_streams::file_stream_writer_c(package_name.c_str());
	if (!write_stream) {
		printf("No write access on %s\n",package_name.c_str());
		return false;
	}

	printf("Total entries found in %s : %llu\n",root_path.c_str(),file_list.size());

	
	write_stream->seek(0);
	uint64_t hdr_size = calc_uncompressed_header_size(file_list);
	{
		//Dummy hdr data - patch later
		uint8_t* dummy = new uint8_t[hdr_size];
		if (!dummy) {
			printf("Out of memory allocating %u bytes for hdr\n",hdr_size);
			delete write_stream;
			return false;
		}
		memset(dummy,0,hdr_size);
		write_stream->write(dummy,hdr_size);
		delete[] dummy;
	}

	//Output
	for (uint32_t i = 0U,j = file_list.size();i < j;++i) {
		read_stream = new file_streams::file_stream_reader_c(file_list[i].c_str());
		if (!read_stream) {
			printf("Failed to open %s\n",file_list[i].c_str());
			printf("Package construction failed\n");
			delete write_stream;
			return false;
		}
 
		pack_ent.addr =  write_stream->tell();
		pack_ent.uncompressed_size = read_stream->size();
		compress(read_stream,write_stream,compression_level);	
		uncomp_size += pack_ent.uncompressed_size;
		entry_list.push_back(pack_ent);
		delete read_stream;
	}
 
	printf("Original entries size : %llu\n",uncomp_size);
	printf("Final pacakge size : %llu (uncomp hdr : %llu)\n",write_stream->tell(),hdr_size);

	//Patch header
	write_stream->seek(0);
	encode(cs_signature,write_stream); //hdr
	encode(file_list.size(),write_stream); //Entries
 
	for (uint32_t i = 0U,j = file_list.size();i < j;++i) {
		encode(entry_list[i].addr,write_stream); 
		encode(entry_list[i].uncompressed_size,write_stream);  
		encode(file_list[i],write_stream);  
	}

	printf("All done\n" );
	delete write_stream;
	return true;
}
 
/*
	Compressed header is placed at pkgsize~(pkgsize..hdr_size)
*/
bool pack_v1(const std::string& package_name,const std::string& root_path,const int32_t compression_level) {
	std::vector<std::string> file_list;
	std::vector<pack_entry_t> entry_list;
	file_streams::file_stream_if* read_stream = 0;
	file_streams::file_stream_if* write_stream = 0;
	uint64_t uncomp_size = 0U;
	pack_entry_t pack_ent;

	printf("Constructing package v1 (compression method : %s)\n",(compression_level == Z_BEST_COMPRESSION) ? "best" : "default");

	get_files(root_path,file_list);
	if (file_list.empty()) {
		printf("Pack : no files in %s\n",root_path.c_str());
		return false;
	}
	entry_list.reserve(file_list.size());

	write_stream = new file_streams::file_stream_writer_c(package_name.c_str());
	if (!write_stream) {
		printf("No write access on %s\n",package_name.c_str());
		return false;
	}

	printf("Total entries found in %s : %llu\n",root_path.c_str(),file_list.size());

	
	write_stream->seek(0);

	encode(cs_signature_v1,write_stream); //hdr
	const uint64_t hdr_jmp_addr = write_stream->tell();
	encode((uint64_t)0U,write_stream); //Dummy hdr offset

	

	//Output
	for (uint32_t i = 0U,j = file_list.size();i < j;++i) {
		read_stream = new file_streams::file_stream_reader_c(file_list[i].c_str());
		if (!read_stream) {
			printf("Failed to open %s\n",file_list[i].c_str());
			printf("Package construction failed\n");
			delete write_stream;
			return false;
		}
 
		pack_ent.addr =  write_stream->tell();
		pack_ent.uncompressed_size = read_stream->size();
		compress(read_stream,write_stream,compression_level);	
		uncomp_size += pack_ent.uncompressed_size;
		entry_list.push_back(pack_ent);
		delete read_stream;
	}
 
	printf("Compressing header...\n");

	//Update header
	const uint64_t tmp = write_stream->tell();
	write_stream->seek(hdr_jmp_addr);
	encode(tmp,write_stream); //patch header offs
	write_stream->seek(tmp);

	//Hdr block size
	const uint64_t uncomp_hdr_sz = calc_uncompressed_header_size(file_list,cs_signature_v1) - ( cs_signature_v1.length() + 1);
	encode(uncomp_hdr_sz,write_stream);		//uncomp hdr sz
 
	const uint64_t  prev_w_offs = write_stream->tell();

	std::vector<uint8_t> hdr_data;
	file_streams::file_stream_if* unc_hdr = new file_streams::file_mem_writer_c(&hdr_data,false);
	file_streams::file_stream_if* unc_hdr_rd;
	encode(file_list.size(),unc_hdr); //Entries
 
	for (uint32_t i = 0U,j = file_list.size();i < j;++i) {
		encode(entry_list[i].addr,unc_hdr); 
		encode(entry_list[i].uncompressed_size,unc_hdr);  
		encode(file_list[i],unc_hdr);
	}

	unc_hdr_rd = new file_streams::file_mem_reader_c(&hdr_data[0],hdr_data.size(),false);
	compress(unc_hdr_rd,write_stream,compression_level);	

	printf("Original entries size : %llu\n",uncomp_size);
	printf("Final pacakge size : %llu (comp hdr : %llu / unc %llu)\n",write_stream->tell(),write_stream->tell() - prev_w_offs,unc_hdr->tell());

	printf("All done\n" );
	delete unc_hdr;
	delete unc_hdr_rd;
	delete write_stream;
	return true;
}

static int32_t compress(file_streams::file_stream_if* source,file_streams::file_stream_if* dest,int32_t level) {
	static const int32_t chunk_size = 16*1024;
    int32_t ret, flush;
    uint32_t have;
    z_stream strm;
    uint8_t in[chunk_size];
    uint8_t out[chunk_size];

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, level);
    if (ret != Z_OK)
        return ret;

    do {
        strm.avail_in = source->read(in,chunk_size);
        flush = source->eof() ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;
 
        do {
            strm.avail_out = chunk_size;
            strm.next_out = out;
            ret = deflate(&strm, flush);
            have = chunk_size - strm.avail_out;
            if (dest->write(out,have)  != have ) { 
                (void)deflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);
    (void)deflateEnd(&strm);
    return Z_OK;
}
 
static void welcome() {
	printf("\n\njpkg(Jimmy's package format)\n");
	printf("Author  : Dimitris Vlachos 2013\n");
	printf("Email   : DimitrisV22@gmail.com\n");
	printf("GitHub  : http://github.com/DimitrisVlachos\n\n");
}

static void help() {
	printf("Usage instructions:\n");
	printf("jpkg package_name.ext directory compress_headers(1/0) compression_level(best/default)\n");
	printf("(Note:Directory recursion is always enabled!)\n");
	printf("\nExample usage:\n");
	printf("jpkg out.pkg filesystem 0 best\n");
	printf("jpkg out.pkg filesystem 0 default\n\n");
	printf("\nExample usage(With compressed headers):\n");
	printf("jpkg out.pkg filesystem 1 best\n");
	printf("jpkg out.pkg filesystem 1 default\n\n");
}

int main(int argc,char** argv) {
	int32_t comp;

	welcome();

	if ((argc < 3) || (argc > 5)) {
		help();
		return 0;
	}

	comp = Z_DEFAULT_COMPRESSION;
	if (argc == 5) {
		std::string tmp = std::string(argv[4]);
		if (tmp == std::string("best"))
			comp = Z_BEST_COMPRESSION;
	}

	if (argv[3][0] == '1')
		pack_v1(argv[1],argv[2],comp);
	else
 		pack(argv[1],argv[2],comp);

	return 0;
}



#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>

#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION  26
#include <fuse.h>

#define BLOCK_SIZE 256
#define MAX_NAME_LENGTH 128
#define DATA_SIZE 254
#define INDEX_SIZE 127
#define NUM_OF_BLOCKS 65536
#define BITVECTOR_SIZE NUM_OF_BLOCKS/8
#define GLOBAL_TABLE_SIZE 65521 // prime number for hashing
#define MAX_OPEN_FILES_PER_PROCESS 16
#define GLOBALVECTOR_SIZE GLOBAL_TABLE_SIZE/8
#define LOCALVECTOR_SIZE MAX_OPEN_FILES_PER_PROCESS/8
#define TOTAL_USERS 6
#define BUFFER_SIZE 254*127

// global table

typedef struct open_file_global_type // elements of the hash table (in-memory "directory")
{
   unsigned short fd; // reference to the file descriptor node
   unsigned short data; // reference to the data or index node (depending on the size)
   mode_t access; // access rights for the file
   unsigned short size; // size of the file
   unsigned short reference_count; // reference count
   struct open_file_global_type *next;
} OPEN_FILE_GLOBAL_TYPE;

OPEN_FILE_GLOBAL_TYPE global_table[GLOBAL_TABLE_SIZE];

// local table

typedef struct open_file_local_type // a node for a local list of open files (per process)
{
   mode_t access_rights; // access rights for this process
   unsigned short global_ref; // reference to the entry for the file in the global table
} OPEN_FILE_LOCAL_TYPE;


OPEN_FILE_LOCAL_TYPE local_table[MAX_OPEN_FILES_PER_PROCESS];

typedef char data_t;
typedef unsigned short index_t;

typedef enum
{
   NIL,
   DIRECT,
   FIL,
   INDEX,
   DATA
} NODE_TYPE;

static const char *NODE_TYPE_STRING[] = {
	"NULL",
	"DIRECT",
	"FILE",
	"INDEX",
	"DATA"
};

static const char *NODE_TYPE_CHAR[] = {
	"-",
	"D",
	"F",
	"I",
	"A"
};

typedef enum
{
  SYSTEM,
  ADMIN,
  NICK_STERN,
  IAN,
  USER3,
  GUEST,
} OWNER;

static const char* OWNER_STRING[] = {
	"SYSTEM",
	"ADMIN",
	"NickStern",
	"Ian", 
	"USER3",
	"GUEST"
};

static const unsigned short OWNER_PERMISSION_LEVEL[] = {
  0,//SYSTEM
  1,//ADMIN
  2,//NICK_STERN
  2,//IAN
  2,//USER3
  3//GUEST
};

typedef struct fs_node
{
   char name[MAX_NAME_LENGTH];
   time_t creat_t; // creation time
   time_t access_t; // last access
   time_t mod_t; // last modification
   mode_t access; // access rights for the file
   unsigned short owner; // owner ID
   unsigned short size;
   index_t block_ref; // reference to the data or index block
} FS_NODE;

typedef struct node
{
   NODE_TYPE type;
   union
   {
      FS_NODE fd;
      data_t data[DATA_SIZE];
      index_t index[INDEX_SIZE];
   } content;
} NODE;

struct LINK{
	NODE *directory;
	struct LINK *next;
	struct LINK *prev; 
};
struct LINK* current;

NODE *curr_directory;//.
NODE *memory; // allocate 2^16 blocks (in init)
//LINK *directory_link;
char *bitVector; // allocate space for managing 2^16 blocks (in init)
char localVector[LOCALVECTOR_SIZE];
char globalVector[GLOBALVECTOR_SIZE];
char *buffer;
unsigned short curr_user = 3;

char filler_sentence[254] = "The full string is worth 254 bytes of data. abcdefghijklmnopqrstuvwxyz 012345.\n This is to show how much of a node is taken. Each node can hold 254 bytes of data.\n Each byte can hold one character. There will be 254 characters per node. This is the END\n";

/*
*Insert at tail:
*Moves new link to current directory at end of doubly linked list, sets current link to new current directory
*Sets new current directory
*/
void InsertAtTail(int mem_address) {
	struct LINK* temp = current;
	struct LINK* newNode = (struct LINK*) malloc(sizeof(struct LINK));
	newNode->directory = &memory[mem_address];
	if(current == NULL) {
		current = newNode;
		return;
	}
	while(temp->next != NULL) temp = temp->next; 
	temp->next = newNode;
	newNode->prev = temp;
	current = newNode;
	curr_directory = &memory[mem_address];
}

/*
*Pop at tail
*Removes tail link, sets current directory to new tail link
*/
void popAtTail(){
	struct LINK* temp = current;
	current = current->prev;
	free(temp);
	current->next = NULL;

	curr_directory = &(*(current->directory));
}

/*
*Change Directory
*Handles changing to new directory.
*/
void change_directory(const char* name){

	if(strcmp(name, ".") ==0){
		curr_directory = curr_directory; //wow
		return;
	}

	if(strcmp(name, "..")==0){
		popAtTail();
		return;
	}

	int index_address = (*curr_directory).content.fd.block_ref;
	for(int i = 0; i < INDEX_SIZE; i++){
		int mem_address = memory[index_address].content.index[i];
		if(strcmp(name, memory[mem_address].content.fd.name) == 0){
			InsertAtTail(mem_address);
			return;
		}
	}
}

/*
*File system create:
*Initializes superblock, bitVector, buffer, directory link list
*/
void file_system_create(){
	//initialize memory
	memory = (NODE *)malloc(BLOCK_SIZE*NUM_OF_BLOCKS);
	bitVector = malloc(BITVECTOR_SIZE);
	buffer = malloc(BUFFER_SIZE);

	curr_directory = &memory[0];

	//setup superblock
	memory[0].type = DIRECT;
	strcpy(memory[0].content.fd.name, "/");
	memory[0].content.fd.creat_t = time(NULL);
	memory[0].content.fd.owner = SYSTEM;
	memory[0].content.fd.size = 0;
	memory[0].content.fd.access = 0755;
	memory[0].content.fd.block_ref = 1;

	memory[1].type = INDEX;
	for(int i = 0; i < INDEX_SIZE ; i++){
		memory[1].content.index[0] = 0x0000;
	}
	bitVector[0] = 0x03;

	current = (struct LINK*) malloc(sizeof(struct LINK));
	current->directory = &memory[0];
	current->prev = current;
}

//Start vector block
int find_space_bitVector(){
	int bitVector_location=0;
	int bitPosition=0;
	for(int i = 0; i < BITVECTOR_SIZE; i++){
		if((unsigned char)bitVector[i] < 255){
			bitVector_location = i;
			unsigned char temp = bitVector[i];

			for(int i = 0; i < 8; i++){
				if((temp & 0x0001) != 0x0001){
					bitPosition = i;
					break;	
				}

				temp = temp >> 1;
			}

			bitVector[i] |= (0x01 << bitPosition);
			break;
		}
	}
	
	return (bitVector_location*8 + bitPosition);
}

int find_space_localVector(){
	int localVector_location=0;
	int localPosition=0;


	unsigned short isFull = 1;
	for(int i = 0; i < LOCALVECTOR_SIZE; i++){
		unsigned short slot = i;
		for(int i = 0; i < 8; i++){
			if(((localVector[slot] >> i) & 1) != 1) isFull = 0;
		}
	}

	if(isFull == 1){
		printf("Proccess table is full.\n");
		return -1;
	}
	
	//place in first empty slot
	for(int i = 0; i < LOCALVECTOR_SIZE; i++){
		if((unsigned char)localVector[i] < 255){
			localVector_location = i;
			unsigned char temp = localVector[i];
			for(int i = 0; i < 8; i++){
				if((temp & 0x0001) != 0x0001){
					localPosition = i;
					break;	
				}
				temp = temp >> 1;
			}

			localVector[i] |= (0x01 << localPosition);
			break;
		}
	}
	return (localVector_location*8 + localPosition);
}

int find_space_globalVector(){
	int globalVector_location=0;
	int globalPosition=0;

	for(int i = 0; i < GLOBALVECTOR_SIZE; i++){
		if((unsigned char)globalVector[i] < 255){
			globalVector_location = i;
			unsigned char temp = globalVector[i];

			for(int i = 0; i < 8; i++){
				if((temp & 0x0001) != 0x0001){
					globalPosition = i;
					break;	
				}

				temp = temp >> 1;
			}

			globalVector[i] |= (0x01 << globalPosition);
			break;
		}
	}
	
	return (globalVector_location*8 + globalPosition);
}
//end vector block


//Start print functions block
void print_bitVector(){
	printf("\n");
	int i;
	printf("Vector[i]    Hex     0 1 2 3 4 5 6 7\n");
	for(i = 0; i < BITVECTOR_SIZE; i++){
		if(bitVector[i] != 0){
			printf("Slot[%4d] : %3d :  ", i, (unsigned char)bitVector[i]);
			printf("|");
			for(int j = 0; j < 8; j++){

				printf("%s|", NODE_TYPE_CHAR[memory[i*8+j].type]);
			}
			printf("\n");
		}
	}
	printf("\n");
}

void print_localVector(){
	printf("\n");
	int i;
	printf("Vector[i]    Hex     0 1 2 3 4 5 6 7\n");
	for(i = 0; i < LOCALVECTOR_SIZE; i++){
		if(localVector[i] != 0){
			printf("Slot[%4d] : %3d :  ", i, (unsigned char)localVector[i]);
			printf("|");
			for(int j = 0; j < 8; j++){

				printf("%s|", NODE_TYPE_CHAR[memory[i*8+j].type]);
			}
			printf("\n");
		}
	}
	printf("\n");
}

void print_globalVector(){
	printf("\n");
	int i;
	printf("Vector[i]    Hex     0 1 2 3 4 5 6 7\n");
	for(i = 0; i < GLOBALVECTOR_SIZE; i++){
		if(globalVector[i] != 0){
			printf("Slot[%4d] : %3d :  ", i, (unsigned char)globalVector[i]);
			printf("|");
			for(int j = 0; j < 8; j++){

				printf("%s|", NODE_TYPE_CHAR[memory[i*8+j].type]);
			}
			printf("\n");
		}
	}
	printf("\n");
}

void print_index(int index_address){
	printf("\n");
	for(int i = 0; i < INDEX_SIZE; i++){
		printf("%d ", memory[index_address].content.index[i]);
	}
	printf("\n");
}

void print_file(){
	
	printf("%s\n", buffer);
}

void print_directory(NODE directory){
	int index_address = directory.content.fd.block_ref;

	printf("Directory: %s\n", directory.content.fd.name);

	printf("Type          Name      Size      Owner                Create Time\n");
	printf("----    ----------   -------   --------   ------------------------\n");

	printf("%4s   %10s   %8d", NODE_TYPE_STRING[directory.type], directory.content.fd.name, directory.content.fd.size);

	printf("     %s", OWNER_STRING[directory.content.fd.owner]);

	char* c_time_string = ctime(&(directory.content.fd.creat_t));
	printf("   %s", c_time_string);

	for(int i = 0; i < INDEX_SIZE; i++){
		int mem_address = memory[index_address].content.index[i];
		if(mem_address != 0){
			printf("%4s   %10s   %8d", NODE_TYPE_STRING[memory[mem_address].type], memory[mem_address].content.fd.name, memory[mem_address].content.fd.size);

			printf("     %s", OWNER_STRING[memory[mem_address].content.fd.owner]);

			char* c_time_string = ctime(&(memory[mem_address].content.fd.creat_t));
			printf("   %s", c_time_string);


		}
	}
}

void print_globalTable(){
	printf("Global Table:\n");
	printf("   n       Name       Addr.     size ref_count\n");
	printf("---- ----------    --------   ------   -------\n");

	for(int i = 0; i < GLOBALVECTOR_SIZE; i++){
		if(globalVector[i] != 0){
			int globalVector_position = i;
			for(int i = 0; i < 8; i++){
				if((1 << i) & globalVector[globalVector_position]){
					printf("%4d %10s   %8d   %6d   %7d\n", globalVector_position*8+i, memory[global_table[globalVector_position*8+i].fd].content.fd.name,  global_table[globalVector_position*8+i].fd,  global_table[globalVector_position*8+i].size,  global_table[globalVector_position*8+i].reference_count);
				}
			}
		}
	}
}

void print_localTable(){
	printf("Local Table:\n");
	printf("   n       Name  global_ref   access\n");
	printf("---- ----------    --------     ----\n");

	for(int i = 0; i < LOCALVECTOR_SIZE; i++){
		if(localVector[i] != 0){
			int localVector_position = i;
			for(int i = 0; i < 8; i++){
				if((1 << i) & localVector[localVector_position]){
					printf("%4d %10s    %8d     %04o\n", localVector_position*8+i, memory[global_table[local_table[localVector_position*8+i].global_ref].fd].content.fd.name,  local_table[localVector_position*8+i].global_ref, local_table[localVector_position*8+i].access_rights);
				}
			}
		}
	}
}
//end print functions block


void addToIndex(int index_address, int mem_address){
	for(int i = 0; i < INDEX_SIZE; i++){
		if(memory[index_address].content.index[i] == 0){
			memory[index_address].content.index[i] = mem_address;
			break;
		}
	}
}

int find_node(NODE *directory, const char* name){
	//if(strcmp(name, directory.content.fd.name) == 0) return 

	int index_address = (*directory).content.fd.block_ref;
	for(int i = 0; i < INDEX_SIZE; i++){
		int mem_address = memory[index_address].content.index[i];
		if(strcmp(memory[mem_address].content.fd.name, name) == 0){
			return mem_address;
		}
	}
	return -1;
}


/*
*Search global table:
*returns index of node on global table
*returns -1 if node not found
*/
int search_globalTable(const char* name){
	for(int i = 0; i < GLOBAL_TABLE_SIZE;i++){
		if(strcmp(memory[global_table[i].fd].content.fd.name, name) == 0){
			return i;
		}
	}	
	return -1;
}

/*
*Search local table:
*returns index of node on local table
*returns -1 if node not found
*/
int search_localTable(const char* name){
	for(int i = 0; i < MAX_OPEN_FILES_PER_PROCESS;i++){
		if(strcmp(memory[global_table[local_table[i].global_ref].fd].content.fd.name, name) == 0){
			return i;
		}
	}
	return -1;
}

/*
*Remove form global table
*Clears data
*Removes global reference from global vector
*/
void remove_from_globalTable(int global_ref){
	global_table[global_ref].fd = 0;
	global_table[global_ref].data = 0; 
	global_table[global_ref].access = 0;
	global_table[global_ref].size = 0; 
	global_table[global_ref].reference_count = 0; 
	global_table[global_ref].next = NULL;	

	int quotient;
	int rem;
	quotient = global_ref /8;
	rem = global_ref % 8;
	globalVector[quotient] ^= (-0 ^ globalVector[quotient]) & (1UL << rem);
}

/*
*Remove form local table
*Clears data
*Removes local reference from local vector
*/
void remove_from_localTable(int local_ref){

	local_table[local_ref].access_rights = 0; // access rights for this process
	local_table[local_ref].global_ref = 0;

	int quotient;
	int rem;
	quotient = local_ref /8;
	rem = local_ref % 8;
	localVector[quotient] ^= (-0 ^ localVector[quotient]) & (1UL << rem);

}

/*
*Add to global table:
*Inserts into first available slot in global vector
*Initializes reference count to 1
*global_table.next is unused due to global vector
*/
unsigned short add_to_globalTable(int mem_address, mode_t access){
	int global_position = find_space_globalVector();
	memory[mem_address].content.fd.access_t = time(NULL);

	global_table[global_position].fd = mem_address;
	global_table[global_position].data = memory[mem_address].content.fd.block_ref; 
	global_table[global_position].access = access;
	global_table[global_position].size = memory[mem_address].content.fd.size; 
	global_table[global_position].reference_count = 1; 
	global_table[global_position].next = NULL;

	return global_position;
}

/*
*Add to process table:
*Inserts new node into first empty slot in local vector
*If node already exists in table, returns
*If local vector is full, returns
*/
void add_to_processTable(unsigned int global_ref, const char* name){
	for(int i = 0; i < LOCALVECTOR_SIZE;i++){
		unsigned short slot = i;
		for(int i = 0; i < 8; i++){
			if(((1 << i) & localVector[slot]) == 1){
				if(local_table[slot*8+i].global_ref == global_ref) return;
			}
		}
	}

	int local_position = find_space_localVector();
	if(local_position == -1) return;

	local_table[local_position].access_rights = global_table[global_ref].access;
	local_table[local_position].global_ref = global_ref; 
}

/*
*File Create:
* Creates a file with a given length
* (* Will create files with filler data to test with read and write)
*/
static int file_create(NODE *directory, int size, const char* name){

	if(find_node(&(*directory), name) != -1 ){
		printf("Fs_Node %s already exists\n", name);
		return -1;
	}

	if(size > DATA_SIZE){
		//FS long
		int file_address, index_address;

		file_address = find_space_bitVector();
		memory[file_address].type = FIL;

		index_address = find_space_bitVector();
		memory[index_address].type = INDEX;

		addToIndex((*directory).content.fd.block_ref, file_address);//link file to directory index
		(*directory).content.fd.size++;

  		strcpy(memory[file_address].content.fd.name, name);
   		memory[file_address].content.fd.creat_t = time(NULL);
		memory[file_address].content.fd.access_t = 0;
		memory[file_address].content.fd.mod_t = 0;
		memory[file_address].content.fd.owner = curr_user;
  		memory[file_address].content.fd.size = size;
  		memory[file_address].content.fd.access = 0755;
		memory[file_address].content.fd.block_ref = index_address;


		while(size > 0){
			int data_address;
			data_address = find_space_bitVector();
			memory[data_address].type = DATA;
			addToIndex(index_address, data_address);
			memset(buffer,0, BUFFER_SIZE);
			for(int i = 0; i < DATA_SIZE; i++){
				buffer[i] = filler_sentence[i];
				size--;
				if(size == 0)break;
			}
			strcpy(memory[data_address].content.data, buffer);
		}
	}else{
		//FS short
		int file_address, data_address;

		file_address = find_space_bitVector();
		memory[file_address].type = FIL;

		data_address = find_space_bitVector();
		memory[data_address].type = DATA;

		addToIndex((*directory).content.fd.block_ref, file_address);
		(*directory).content.fd.size++;

  		strcpy(memory[file_address].content.fd.name, name);
   		memory[file_address].content.fd.creat_t = time(NULL);
		memory[file_address].content.fd.access_t = 0;
		memory[file_address].content.fd.mod_t = 0;
   		memory[file_address].content.fd.owner = curr_user;
   		memory[file_address].content.fd.size = size;
   		memory[file_address].content.fd.access = 0755;
		memory[file_address].content.fd.block_ref = data_address;
		memset(buffer,0, BUFFER_SIZE);
		for(int i = 0; i < size; i++){
			buffer[i] = filler_sentence[i];
		}
		strcpy(memory[data_address].content.data, buffer);
	}
	
	return 0;
}

void node_delete(NODE *node, int mem_address){
   	(*node).type = NIL;
   	strcpy((*node).content.fd.name, "");
   	(*node).content.fd.creat_t = 0;
   	(*node).content.fd.owner = 0;
   	(*node).content.fd.size = 0;
   	(*node).content.fd.access = 0;

	for(int i = 0; i < INDEX_SIZE; i++){
		(*node).content.index[i] = 0;
	}
	for(int i = 0; i < DATA_SIZE; i++){
		(*node).content.data[i] = 0;
	}

	int quotient;
	int rem;
	quotient = mem_address /8;
	rem = mem_address % 8;
	bitVector[quotient]  ^= (-0 ^ bitVector[quotient]) & (1UL << rem);
}

void file_delete(NODE *directory, const char* name){
	
	int file_address = 0, index_address = 0;
	index_address = (*directory).content.fd.block_ref;
	for(int i = 0; i < INDEX_SIZE ; i++){
		
		if(strcmp(memory[memory[index_address].content.index[i]].content.fd.name, name) == 0){
			file_address = memory[index_address].content.index[i];
			break;
		}	
	}

	if(file_address == 0){
		printf("Could not find file\n");
		return;
	}

	index_address = memory[file_address].content.fd.block_ref;
	for(int i = 0; i < INDEX_SIZE; i++){
		int data_address;
		if(memory[index_address].content.index[i] != 0){
			data_address = memory[index_address].content.index[i];
			node_delete(&memory[data_address], data_address);
		}
	}

	for(int i = 0; i < INDEX_SIZE; i++){
		if(memory[(*directory).content.fd.block_ref].content.index[i] == file_address){
			memory[(*directory).content.fd.block_ref].content.index[i] = 0;
		}
	}

	(*directory).content.fd.size--;
	node_delete(&memory[index_address], index_address);
	node_delete(&memory[file_address], file_address);

}
/*
*Directory Create:
*Creates a directory in the current directory
*/
static int directory_create(NODE *directory, const char* name){

	if(find_node(&(*directory), name) != -1 ){
		printf("Fs_Node %s already exists\n", name);
		return -1;
	}

	int dir_address, index_address;

	dir_address = find_space_bitVector();
	memory[dir_address].type = DIRECT;

	index_address = find_space_bitVector();
	memory[index_address].type = INDEX;

	addToIndex((*directory).content.fd.block_ref, dir_address);
	(*directory).content.fd.size++;


	//addToIndex(index_address, dir_address);


  	strcpy(memory[dir_address].content.fd.name, name);
   	memory[dir_address].content.fd.creat_t = time(NULL);
	memory[dir_address].content.fd.access_t = 0;
	memory[dir_address].content.fd.mod_t = 0;
   	memory[dir_address].content.fd.owner = curr_user;
   	memory[dir_address].content.fd.size = 0;
   	memory[dir_address].content.fd.access = 0755;
	memory[dir_address].content.fd.block_ref = index_address;


	return 0;
}

/*
*Directory delete helper function
*Recursive iteration over all sub-directories
*/
void recursive_delete(NODE *parent, int dir_address){	
	for(int i = 0; i < INDEX_SIZE; i++){
		int data_address = memory[(*parent).content.fd.block_ref].content.index[i];
		if(data_address != 0){
			int type = memory[data_address].type;

			if(type == 1){
				recursive_delete(&memory[data_address], data_address);
			}else if(type == 2){
				file_delete(&(*parent), memory[data_address].content.fd.name);
			}else{

			}
		}
	}

	node_delete(&memory[(*parent).content.fd.block_ref], (*parent).content.fd.block_ref);
	node_delete(&memory[dir_address], dir_address);
}

/*
*Directory delet:
*Removes directory, along with all sub-directories and files
*/
static int directory_delete(NODE *directory, const char* name){

	int dir_address, index_address;
	index_address = (*directory).content.fd.block_ref;
	for(int i = 0; i < INDEX_SIZE ; i++){
		if(strcmp(memory[memory[index_address].content.index[i]].content.fd.name , name) == 0){
			dir_address = memory[index_address].content.index[i];
			break;
		}	
	}

	if(dir_address == 0){
		printf("Could not find dir\n");
		return -1;
	}

	index_address = memory[dir_address].content.fd.block_ref;

	recursive_delete(&memory[dir_address], dir_address);
	(*directory).content.fd.size--;
	
	for(int i = 0; i < INDEX_SIZE; i++){
		if(memory[(*directory).content.fd.block_ref].content.index[i] == dir_address){
			memory[(*directory).content.fd.block_ref].content.index[i] = 0;
		}
	}
	return 0;	
}

/*
*Evaluate access codes
*Compares access request against files permissions
*Return 1 if does not have access, else return 0
*/
unsigned short evaluate_user_access(mode_t open_access, mode_t node_access, int owner){
	unsigned short hasAccess = 0; // 0 == TRUE, 1 == FALSE
	if(OWNER_PERMISSION_LEVEL[curr_user] == OWNER_PERMISSION_LEVEL[owner]){ //OWNER
		if(strcmp(OWNER_STRING[curr_user], OWNER_STRING[owner]) == 0){
			if(1 == ((open_access >> 8) & 1U)){ //READ
				if(1 != ((node_access >> 8) & 1U)){
					hasAccess = 1;
				}
			}
			if(1 == ((open_access >> 7) & 1U)){ //WRITE
				if(1 != ((node_access >> 7) & 1U)){
					hasAccess = 1;
				}
			}
			if(1 == ((open_access >> 6) & 1U)){ //EXECUTE
				if(1 != ((node_access >> 6) & 1U)){
					hasAccess = 1;
				}
			}
		}else{ //GROUP
			if(1 == ((open_access >> 5) & 1U)){ //READ
				if(1 != ((node_access >> 5) & 1U)){
					hasAccess = 1;
				}
			}
			if(1 == ((open_access >> 4) & 1U)){ //WRITE
				if(1 != ((node_access >> 4) & 1U)){
					hasAccess = 1;
				}
			}
			if(1 == ((open_access >> 3) & 1U)){ //EXECUTE
				if(1 != ((node_access >> 3) & 1U)){
					hasAccess = 1;
				}
			}
		}
	}else{ //OTHER
		if(1 == ((open_access >> 2) & 1U)){ //READ
			if(1 != ((node_access >> 2) & 1U)){
				hasAccess = 1;
			}
		}
		if(1 == ((open_access >> 1) & 1U)){ //WRITE
			if(1 != ((node_access >> 1) & 1U)){
				hasAccess = 1;
			}
		}
		if(1 == ((open_access >> 0) & 1U)){ //EXECUTE
			if(1 != ((node_access >> 0) & 1U)){
				hasAccess = 1;
			}
		}
	}
	if(hasAccess == 1) printf("Access invalid.\n");
	return hasAccess;
}



static int
single_getattr(const char *path, struct stat *stbuf)
{
	fprintf(stderr, "GETATTR %s\n", path);

	print_directory(*curr_directory);
   	print_bitVector();
   	print_localTable();
    	print_globalTable();

	memset(stbuf, 0, sizeof(struct stat));
	//if (strcmp(path, "/") == 0) { /* The root directory of our file system. */

	if(strcmp(path, (*curr_directory).content.fd.name) == 0){
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = (*curr_directory).content.fd.size+2;
		return 0;
	}

	int mem_address = find_node(&(*curr_directory), path);
	if(mem_address != -1){
		if(memory[mem_address].type == DIRECT) {
			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = memory[mem_address].content.fd.size+2;
		}else{
			stbuf->st_mode = S_IFREG | 0755;
			stbuf->st_nlink = 1;
			stbuf->st_size = memory[mem_address].content.fd.size;
		}
        } else {
		return -ENOENT;
	}
	return 0;
}

static int
single_open(const char *path, struct fuse_file_info *fi)
{
    fprintf(stderr, "OPEN\n");

	int mem_address = find_node(&(*curr_directory), path);
	if(mem_address == -1){
		printf("Could not open %s. \n", path);
		return -ENOENT;
	}

	if(evaluate_user_access(fi->flags, memory[mem_address].content.fd.access, memory[mem_address].content.fd.owner) == 0){//TODO evluated flags
		int global_ref = search_globalTable(path);
		if(global_ref == -1){
			global_ref = add_to_globalTable(mem_address, 0755);
			add_to_processTable(global_ref, path);
		}else{
			if(evaluate_user_access(fi->flags, global_table[global_ref].access, memory[mem_address].content.fd.owner) == 0){//TODO evaluate flags
				global_table[global_ref].reference_count++;
				add_to_processTable(global_ref, path);
			}
		}
	}

	print_directory(*curr_directory);
	print_bitVector();
	print_localTable();
	print_globalTable();

    return 0;
}

static int
single_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
              off_t offset, struct fuse_file_info *fi)
{
	fprintf(stderr, "\nREADDIR %s\n", path);

	print_directory(*curr_directory);
   	print_bitVector();
   	print_localTable();
    	print_globalTable();

	if((*curr_directory).type == DIRECT){
		filler(buf, ".", NULL, offset);           // current directory (.)
		filler(buf, "..", NULL, offset);          // parent directory (..)
	}
    
	int truncate_path = 0;
	if(strcmp((*curr_directory).content.fd.name, "/") == 0){ //truncate only "/"
		truncate_path = 1;
	}else{ //truncate directory path + intial "/"
		truncate_path = strlen(path)+1;
	}

	unsigned short index_address = (*curr_directory).content.fd.block_ref;
	for(int i = 0; i < (*curr_directory).content.fd.size; i++){
		unsigned short data_address = memory[index_address].content.index[i];
		if(memory[data_address].type == DIRECT){
			filler(buf, memory[data_address].content.fd.name+truncate_path, NULL, offset);
		}else{
			filler(buf, memory[data_address].content.fd.name+truncate_path, NULL, offset);
		}
	}

	return 0;
}

void file_close(const char* name){
	int local_ref = -1;
	int global_ref = -1;

	local_ref = search_localTable(name);
	remove_from_localTable(local_ref);

	global_ref = search_globalTable(name);
	if(global_ref == -1){
		return;
	}	

	global_table[global_ref].reference_count--;
	if(global_table[global_ref].reference_count == 0){
		remove_from_globalTable(global_ref);
	}
}

static int
single_read(const char *path, char *buf, size_t size, off_t offset,
           struct fuse_file_info *fi)
{
    fprintf(stderr, "READ\n");

	int file_address = -1;
	int local_address = -1;
	int global_address = -1;

	local_address = search_localTable(path);
	if(local_address == -1){
		return -ENOENT;
	}
	global_address = local_table[local_address].global_ref;
	if(global_address == -1){
		printf("\n\n\n\nFile not open.\n\n\n\n\n");
		return -ENOENT;
	}
	file_address = global_table[global_address].fd;



	if(memory[file_address].type == DIRECT){
		printf("\n\n\n\n\n\n\n\nCannot read from directory.\n\n\n\n\n\n");
		return size;
	}
	
	int index_address = memory[file_address].content.fd.block_ref;
	memory[file_address].content.fd.access_t = time(NULL);

	memset(buffer,0, BUFFER_SIZE);
		printf("\n\n\n\n READ %s %lu\n\n\n", buf, size);
	if(memory[file_address].content.fd.size < 255){
		int data_address = memory[file_address].content.fd.block_ref;

		memcpy(buf, memory[data_address].content.data, memory[file_address].content.fd.size); // provide the content to the caller
	}else{
		int data_address = memory[index_address].content.index[0];
		strcpy(buffer, memory[data_address].content.data);

		for(int i = 1 ; i < INDEX_SIZE; i++){
			if(memory[index_address].content.index[i] == 0) break;
			int data_address = memory[index_address].content.index[i];
			strcat(buffer, memory[data_address].content.data);
		}

		//printf("\n\n\n\n\n\nhere%s\n\n\n\n\n\n", buffer);
		memcpy(buf, buffer, memory[file_address].content.fd.size);
	}
	print_directory(*curr_directory);
	print_bitVector();

	return size;
}

static int
single_write(const char *path, const char *buf, size_t size, off_t offset,
	     struct fuse_file_info *fi)
{
 	fprintf(stderr, "WRITE\n");

	printf("\n\n\n\nWRITE %s %s %lu\n\n\n\n\n\n", path, buf, size);

	int file_address = -1;
	int local_address = -1;
	int global_address = -1;

	local_address = search_localTable(path);
	if(local_address == -1){
		printf("File not open.\n");
		return -ENOENT;
	}
	global_address = local_table[local_address].global_ref;
	if(global_address == -1){
		return -ENOENT;
	}
	file_address = global_table[global_address].fd;

	if(memory[file_address].content.fd.size+size < 255){
		int data_address = memory[file_address].content.fd.block_ref;

		memset(buffer,0, BUFFER_SIZE);
		strcpy(buffer, memory[data_address].content.data);
		//printf("%s\n", memory[data_address].content.data);
		strcat(memory[data_address].content.data, buf);
		memory[file_address].content.fd.size += size;
	}else{
		int index_address = memory[file_address].content.fd.block_ref;
		int tmp_size = size;

		if(memory[index_address].type == DATA){ //create index node if none exists
			int data_address = index_address;
			index_address = find_space_bitVector();
			memory[index_address].type = INDEX;
			addToIndex(index_address, data_address);
			memory[file_address].content.fd.block_ref = index_address;
		}
		
		memset(buffer,0, BUFFER_SIZE);
		int iteration_offset = 0;
		while(tmp_size > 0){
			int data_address;
			data_address = find_space_bitVector();
			memory[data_address].type = DATA;
			addToIndex(index_address, data_address);
			memset(buffer,0, BUFFER_SIZE);
			for(int i = 0; i < DATA_SIZE; i++){
				buffer[i] = buf[i+(iteration_offset*DATA_SIZE)];
				tmp_size--;
				if(tmp_size == 0)break;
			}
			iteration_offset++;
			//printf("\n\n\n\n\n\nhere%s\n\n\n\n\n\n", buffer);
			strcpy(memory[data_address].content.data, buffer);
		}

		memory[file_address].content.fd.size += size;
	}
	print_directory(*curr_directory);
	print_bitVector();
	return size;
}



static int
single_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
 	fprintf(stderr, "CREATE %s\n", path);

	int res;

	res = file_create(&(*curr_directory), 0, path);  
	if (res == -1) return -errno;


	print_directory(*curr_directory);
	print_bitVector();
	return 0;
}

static int
single_mknod(const char *path, mode_t mode, dev_t dev)
{
    fprintf(stderr, "MKNOD\n");

    if (S_ISREG(mode)) // only regular files allowed
        single_create(path, mode, NULL);
	  else
	      return -EINVAL;
	      
	  return 0;
}

static int
single_unlink(const char *path)
{
    fprintf(stderr, "UNLINK\n");
    
	int mem_address = find_node(&(*curr_directory), path);

	if (mem_address == -1) return -errno;

	if(memory[mem_address].type == FIL){
		file_delete(&(*curr_directory), path);
	}else if(memory[mem_address].type == DIRECT){
		directory_delete(&(*curr_directory), path);
	}else{

	}

	print_directory(*curr_directory);
	print_bitVector();
	return 0;
}

static int
single_utime(const char *path, struct utimbuf *ubuf)
{
    fprintf(stderr, "UTIME\n");

    return 0;
}

static int
single_truncate(const char *path, off_t newsize)
{
    fprintf(stderr, "TRUNCATE\n");
        
    return 0;
}

static int
single_getxattr(const char *path, const char *name, char *value, size_t vlen)
{
    fprintf(stderr, "GETXATTR: %s\n", name);
        
    return 0;
}
        
static int
single_flush(const char *path, struct fuse_file_info *fi)
{
    fprintf(stderr, "FLUSH\n");
        
    return 0;
}

static int
single_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    fprintf(stderr, "FSYNC\n");
        
    return 0;
}

static int
single_statfs(const char *path, struct statvfs *statv)
{
    fprintf(stderr, "STATFS\n");
        
    return 0;
}

static int
single_access(const char *path, int mask)
{
	fprintf(stderr, "\n\nACCESS: %s %x\n\n", path, mask);
    

	if(strcmp(path, (*curr_directory).content.fd.name) == 0){// cd .
		change_directory(".");
	}else if(strcmp(path, (*(current->prev->directory)).content.fd.name) == 0){ // cd ..
		change_directory("..");
	}else{
		int mem_address = find_node(&(*curr_directory), path);
		if(mem_address == -1) return -ENOENT;
		if(memory[mem_address].type == DIRECT){
			change_directory(path);
		}else{

		}
	}
	return 0;
}

static int
single_release(const char *path, struct fuse_file_info *fi)
{
	fprintf(stderr, "RELEASE\n");
	file_close(path);
	return 0;
}

static int single_rmdir(const char *path)
{
	int res;	
	res = directory_delete(&(*curr_directory), path);
	if (res == -1) return -errno;
	return 0;
}

static int single_mkdir(const char *path, mode_t mode)
{
	int res;

	res = directory_create(&(*curr_directory), path);
		
	if (res == -1) return -errno;

	return 0;
}

static struct fuse_operations single_filesystem_operations = {
    .getattr  = single_getattr,
    .getxattr = single_getxattr,
    .create   = single_create,
    .mknod    = single_mknod,
    .mkdir    = single_mkdir,
    .rmdir    = single_rmdir,
    .utime    = single_utime,
    .unlink   = single_unlink,
    .truncate = single_truncate,
    .flush    = single_flush,
    .fsync    = single_fsync,
    .access   = single_access,
    .open     = single_open,
    .statfs   = single_statfs,
    .release  = single_release,
    .read     = single_read,
    .write    = single_write,
    .readdir  = single_readdir,
};

int main(int argc, char **argv){
	file_system_create();
	return fuse_main(argc, argv, &single_filesystem_operations, NULL);
}

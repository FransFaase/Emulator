/*
	http://ref.x86asm.net/geek.html
	https://www.felixcloutier.com/x86/
	https://faculty.nps.edu/cseagle/assembly/sys_call.html
	https://syscalls.mebeim.net/?table=x86/64/x64/latest
*/
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdint>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>

char *copystr(const char *v)
{
	char *result = (char*)malloc(strlen(v) + 1);
	strcpy(result, v);
	return result;
}

#define MAX_NR_MESSAGES 200
#define MAX_MESSAGE_LEN 200
char messages[MAX_NR_MESSAGES][MAX_MESSAGE_LEN];
int message_nr = 0;
bool message_round = false;

int indent_depth = 0;
void indent(FILE *fout) { fprintf(fout, "%*.*s", indent_depth, indent_depth, ""); }

FILE *log_file = stdout;
FILE *stat_file = 0;

#ifdef ENABLE_DO_TRACE
#define DO_TRACE(...) if (do_trace) trace(__VA_ARGS__)
bool do_trace = false;
bool do_trace_call = false;
bool out_trace = false;
int nr_ret;

void trace(const char* format, ...)
{
	va_list argp;
	va_start(argp, format);
	char *s = messages[message_nr];
	if (indent_depth < 20)
	{
		for (int i = 0; i < indent_depth; i++)
			*s++ = ' ';
		vsnprintf(s, MAX_MESSAGE_LEN - indent_depth, format, argp);
	}
	else
	{
		snprintf(s, 20, "(%d) ", indent_depth);
		s[20] = '\0';
		s += strlen(s);	
		vsnprintf(s, MAX_MESSAGE_LEN - 20, format, argp);
	}
	messages[message_nr][MAX_MESSAGE_LEN-1] = '\0';
	if (out_trace)
		fprintf(log_file, "%s", messages[message_nr]);
	if (++message_nr >= MAX_NR_MESSAGES)
	{
		message_nr = 0;
		message_round = true;
	}
	va_end(argp);
}

void trace_ni(const char* format, ...)
{
	va_list argp;
	va_start(argp, format);
	char *s = messages[message_nr];
	vsnprintf(s, MAX_MESSAGE_LEN - indent_depth, format, argp);
	messages[message_nr][MAX_MESSAGE_LEN-1] = '\0';
	if (out_trace)
		fprintf(log_file, "%s", messages[message_nr]);
	if (++message_nr >= MAX_NR_MESSAGES)
	{
		message_nr = 0;
		message_round = true;
	}
	va_end(argp);
}

void print_trace(FILE *f)
{
	if (out_trace)
		return;
	fprintf(log_file, "---\n");
	if (message_round)
		for (int i = message_nr; i < MAX_NR_MESSAGES; i++)
			fprintf(log_file, "%s", messages[i]);
	for (int i = 0; i < message_nr; i++)
		fprintf(log_file, "%s", messages[i]);
	fprintf(log_file, "---\n");
}

#else
#define DO_TRACE(...) 
#endif

#ifdef TRACE_MEMORY
bool trace_mem = false;
#endif

#ifdef TRACE_WRITES

bool trace_writes = true;
uint32_t write_handle = 0;

void close_write()
{
	if (write_handle > 2)
		fprintf(log_file, "'\n");
	write_handle = 0;
}

void write_char(uint32_t h, uint32_t ch)
{
	if (h <= 2)
	{
		close_write();
		write_handle = 0;
		return;
	}
	if (write_handle != h)
	{
		close_write();
		fprintf(log_file, "\nwrite to %d: '", h);
		write_handle = h;
	}
	ch = ch & 0xFF;
	if ((' ' <= ch && ch < 127) || ch == '\n' || ch == '\t') 
		fprintf(log_file, "%c", ch);
	else
		fprintf(log_file, "\\%02x", ch);
}
#define CLOSE_WRITE if (trace_writes) close_write();
#define WRITE_CHAR(H,C) if (trace_writes) write_char(H, C);
#else
#define CLOSE_WRITE
#define WRITE_CHAR(H,C)
#endif


int gen_program_for = -1;

typedef unsigned char byte;

bool do_gen = false;

struct FunctionCall
{
	uint32_t addr;
	FunctionCall *next;
	
	FunctionCall(uint32_t a, FunctionCall *n) : addr(a), next(n) {}
};


struct Statement
{
	int function_enter;
	bool dyn_func;
	int label_pos;
	bool is_loop;
	char kind;
	const char *code;
	uint32_t val32;
	uint16_t val16;
	byte val8;
	FunctionCall *func_calls;
	const char *call_reg;
	char gen_state;
	
	Statement()
	{
		function_enter = 0;
		dyn_func = false;
		label_pos = 0;
		is_loop = false;
		code = 0;
		val32 = 0;
		val16 = 0;
		val8 = 0;
		func_calls = 0;
		call_reg = 0;
	}

	void call(uint32_t addr, const char *reg = 0)
	{
		kind = 'c';
		call_reg = reg;
		for (FunctionCall *func_call = func_calls; func_call != 0; func_call = func_call->next)
			if (func_call->addr == addr)
				return;
		func_calls = new FunctionCall(addr, func_calls);
	}
};

Statement dummy;

Statement *cur_stat = &dummy;

uint32_t start_code;
uint32_t start_pc;
uint32_t end_code;
Statement **statements = 0;

void init_statements(uint32_t start, uint32_t end)
{
	do_gen = true;
	start_code = start;
	end_code = end;
	statements = (Statement**)malloc((end_code - start_code) * sizeof(Statement*));
	for (uint32_t i = 0; i < end_code - start_code; i++)
		statements[i] = 0;
}

void start_inst(uint32_t pc)
{
	if (statements != 0 && start_code <= pc && pc < end_code)
	{
		uint32_t offset = pc - start_code;
		if (statements[offset] == 0)
			statements[offset] = new Statement;
		cur_stat = statements[offset];
	}
}

#define START_INST(X) start_inst(X)
#define CODE(X) \
	cur_stat->kind = 's'; \
	cur_stat->code = #X; \
	X
#define CODE_V8(X) \
    cur_stat->kind = 'b'; \
    cur_stat->code = #X; \
    cur_stat->val8 = val8; \
    X
#define CODE_V16(X) \
    cur_stat->kind = 'w'; \
    cur_stat->code = #X; \
    cur_stat->val16 = val16; \
    X
#define CODE_V32(X) \
    cur_stat->kind = 'l'; \
    cur_stat->code = #X; \
    cur_stat->val32 = val32; \
    X
#define CODE_JUMP(C,J) \
    cur_stat->kind = 'j'; \
	cur_stat->code = #C; \
	if (C) \
	{ \
		cur_stat->val32 = _pc = J; \
		DO_TRACE("  => jump to %08x\n\n", _pc); \
	}
#define CODE_CALL(X,R) cur_stat->call(X,R)
#define CODE_RETURN cur_stat->kind = 'r'
#define CODE_INT cur_stat->kind = 'i'; cur_stat->val32 = _eax


#define MAX_NR_SKIP_PROCESSES 4000
struct
{
	int nr;
	int nr_sub_processes;
	uint32_t exit_value;
} skipped_processes[MAX_NR_SKIP_PROCESSES];
int nr_skipped_processes = 0;

FILE *f_skip = 0;

void init_skip_processes()
{
	FILE *f = fopen("skip_processes.txt", "r");
	if (f != 0)
	{
		char buffer[40];
		while (fgets(buffer, 39, f))
		{
			sscanf(buffer, "%d %d %x",
				&skipped_processes[nr_skipped_processes].nr, 
				&skipped_processes[nr_skipped_processes].nr_sub_processes,
				&skipped_processes[nr_skipped_processes].exit_value);
			nr_skipped_processes++;
		}
		fclose(f);
	}
	f_skip = fopen("skip_processes_new.txt", "w");
}

bool skip_process(int nr, int &nr_sub_processes, uint32_t &exit_value)
{
	for (int i = 0; i < nr_skipped_processes; i++)
		if (skipped_processes[i].nr == nr)
		{
			nr_sub_processes = skipped_processes[i].nr_sub_processes;
			exit_value = skipped_processes[i].exit_value;
			return true;
		}
	return false;
}

void completed_process(int nr, int nr_sub_processes, uint32_t exit_value)
{
	if (f_skip != 0)
	{
		fprintf(f_skip, "%d %d %x\n", nr, nr_sub_processes, exit_value);
		fflush(f_skip);
	}
}

const char *source_dir = 0;

bool mapped_in_source(char *filename) { return strncmp(filename, "result/", 7) != 0; } 

const char* map_file(char *name, bool read_only)
{
	static char fullname[500];
	if (strncmp(name, "external/distfiles/", 19) == 0)
		name += 9;
	if (name[0] == '/')
		name++;
	if (read_only)
	{
		static const char *paths[] = { "replacement/", "result/", "*seed/", "*seed/stage0-posix/", "*"};
		bool found = false;
		for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++)
		{
			if (paths[i][0] == '*')
			{
				strcpy(fullname, source_dir);
				strcat(fullname, paths[i] + 1);
			}
			else
				strcpy(fullname, paths[i]);
			strcat(fullname, name);
			if (access(fullname, R_OK) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			strcpy(fullname, "result/");
			strcat(fullname, name);
		}
	}
	else
	{
		strcpy(fullname, "result/");
		strcat(fullname, name);
		
		for (char *s = fullname; *s != '\0'; s++)
			if (*s == '/')
			{
				*s = '\0';
				if (access(fullname, F_OK) == -1)
					mkdir(fullname, 0700);
				*s = '/';
			}
	}
	
	return fullname;
}	

class Process;
class Usage;

class File
{
public:
	char *name;

	int nr;
	
	bool produced;
	Process *produced_by;
	bool exec;
	bool used_as_input;
	bool removed;
	
	int fh; // -1 => closed

	Usage *usages;

	File *next;
	
	File(const char *fn, int _nr) : nr(_nr), produced(false), produced_by(0), exec(false), used_as_input(0), removed(0), fh(-1), usages(0), next(0), mapped_path(0)
	{
		name = copystr(fn);
	}
	
	const char *getMappedPath(bool read_only)
	{
		if (mapped_path == 0 || (!read_only && mapped_in_source(mapped_path)))
			mapped_path = copystr(map_file(name, read_only));
		fprintf(log_file, "File %s mapped on %s\n", name, mapped_path);
		return mapped_path;
	}
	
	const char *mappedPath() { return mapped_path == 0 ? "<NULL>" : mapped_path; }
	
private:
	char *mapped_path;
};

File *files = 0;

File *getFile(const char *name)
{
	static int nr = 1;
	if (name[0] == '.' && name[1] == '/')
		name += 2;
	else if (name[0] == '/')
		name++;
	File **ref_file = &files;
	for (; *ref_file != 0; ref_file = &(*ref_file)->next)
		if (strcmp((*ref_file)->name, name) == 0)
			return *ref_file;
	File *file = new File(name, nr++);
	*ref_file = file;
	ref_file = &(*ref_file)->next;
	return file;
}

class FunctionName
{
public:
	uint32_t addr;
	char name[50];
	FunctionName *next;
	FunctionName(uint32_t a, char *n, FunctionName *nn)
	{
		addr = a;
		strcpy(name, n);
		next = nn;
	}
};

#ifdef CATCH_SEGFAULT
#ifdef ENABLE_DO_TRACE
#define CHECK_MEM(A) if (memory[(uint16_t)(A >> 16)] == 0) { print_trace(log_file); fprintf(log_file, "Segmentation fault at %x\n", A); exit(0); }
#else
#define CHECK_MEM(A) if (memory[(uint16_t)(A >> 16)] == 0) { fprintf(log_file, "Segmentation fault at %x\n", A); exit(0); }
#endif
#else
#define CHECK_MEM(A)
#endif

#define CALL_STACK_CHECK
#ifdef CALL_STACK_CHECK
uint32_t call_stack[100];
int call_stack_depth = 0;
uint32_t call_stack_top = 0xffffffff; 

void call_stack_call(uint32_t sp)
{
	call_stack_top = call_stack[call_stack_depth++] = sp;
}

void call_stack_return(uint32_t sp)
{
	call_stack_depth--;
	if (call_stack[call_stack_depth] != sp - 4)
	{
		fprintf(log_file, "Ret %x does not match call %x\n", sp - 4, call_stack[call_stack_depth]);
		
		exit(1);
	}
	call_stack_top = call_stack_depth > 0 ? call_stack[call_stack_depth - 1] : 0xffffffff; 
}

#define CALL_STACK_CALL call_stack_call(_process->sp);
#define CALL_STACK_RETURN call_stack_return(_process->sp);
#else
#define CALL_STACK_CALL
#define CALL_STACK_RETURN
#endif

class Process
{
public:
	const char *name;
	int nr;
	int argc;
	char **argv;
	char **env;
	
	byte *memory[0x10000];
	
	uint32_t pc;
	uint32_t sp;
	uint32_t start_code;
	uint32_t end_code;
	uint32_t brk;
	Process *parent;
	int nr_sub_processes;

	// Saved registers
	uint32_t _ebx;
	uint32_t _ecx;
	uint32_t _edx;
	uint32_t _esi;
	uint32_t _edi;
	uint32_t _ebp;
	int32_t _flags;
	
	Usage *uses;
	
	FunctionName *functionNames;
	
	Process *next;
	
	static int nr_of_processes;
	
	Process(Process *_parent = 0) : parent(_parent), nr_sub_processes(0), uses(0), functionNames(0), next(0)
	{
		name = "";
		
		nr = ++nr_of_processes;
		
		for (int i = 0; i < 0x10000; i++)
			memory[i] = 0;
		sp = 0L;
		_sp_allocated = 0xffff0000;
		storeAllocByte(_sp_allocated, 0);
		
		if (parent != 0)
		{
			for (Process *p = parent; p != 0; p = p->parent)
				p->nr_sub_processes++;
			pc = parent->pc;
			sp = parent->sp;
			end_code = parent->end_code;
			brk = parent->brk;
			for (int i = 0; i < 0x10000; i++)
				if (parent->memory[i] != 0)
				{
					memory[i] = (byte*)malloc(0x10000 * sizeof(byte));
					for (int j = 0; j < 0x10000; j++)
						memory[i][j] = parent->memory[i][j];
				}
		}
	}
	~Process()
	{
		for (int i = 0; i < 0x10000; i++)
			if (memory[i] != 0)
				free(memory[i]);
		for (FunctionName *functionName = functionNames; functionName != 0;)
		{
			FunctionName *next = functionName->next;
			delete functionName;
			functionName = next;
		}
	}

	void init(int argc, char *argv[], char *env[])
	{
		name = argv[0];
		this->argc = argc;
		this->argv = argv;
		this->env = env;
		
		int nr_env = 0;
		while (env[nr_env] != 0)
			nr_env++;
		
		uint32_t *addrs = new uint32_t[argc + nr_env + 2];
		int v = 0;

		// Push environment in reverse order on the stack
		addrs[v++] = 0;
		for (int i = nr_env - 1; i >= 0; i--)
		{
			for (int j = strlen(env[i]); j >= 0; j--)
				pushByte(env[i][j]);
			addrs[v++] = sp;
		}
		
		// Push arguments in reverse order on the stack
		addrs[v++] = 0;
		for (int i = argc - 1; i >= 0; i--)
		{
			for (int j = strlen(argv[i]); j >= 0; j--)
				pushByte(argv[i][j]);
			addrs[v++] = sp;
		}

		// Align stack pointer to double word boundary
		while (sp % 4 != 0)
			pushByte(0);
		
		// Push the collected addresses to the stack
		for (int i = 0; i < v; i++)
			push(addrs[i]);
		delete[] addrs;

		// Push argc to the stack
		push(argc);
	}
	
	void finish();

private:	
	uint32_t _sp_allocated;

public:
	
	byte loadByte(uint32_t address)
	{
		CHECK_MEM(address)
#ifdef TRACE_MEMORY
		byte result = memory[(uint16_t)(address >> 16)][(address & 0xffff)];
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("Load %02x from %08x\n", result, address);
			else
#endif
			fprintf(log_file, "Load %02x from %08x\n", result, address);
		}
		return result;
#else
		return memory[(uint16_t)(address >> 16)][(address & 0xffff)];
#endif
	}
				
	void storeAllocByte(uint32_t address, byte value)
	{
		uint16_t hi = (uint16_t)(address >> 16);
		if (memory[hi] == 0)
			memory[hi] = (byte*)malloc(0x10000 * sizeof(byte));
		memory[hi][(address & 0xffff)] = value;
	}

	void storeByte(uint32_t address, byte value)
	{
		CHECK_MEM(address)
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("Store %02x at %08x\n", value, address);
			else
#endif
			fprintf(log_file, "Store %02x at %08x\n", value, address);
		}
#endif
#ifdef ENABLE_DO_TRACE
		if (address == 0x080500e4 && value == 1)
		{
			printf("Assign 0x080500e4 with 1\n");
			print_trace(log_file);
			exit(1);
		}
#endif
		memory[(uint16_t)(address >> 16)][(address & 0xffff)] = value;
	}

	uint32_t loadDWord(uint32_t address)
	{
		CHECK_MEM(address)
		if ((address & 0x3) == 0)
		{
#ifdef TRACE_MEMORY
			uint32_t value = *(uint32_t*)(memory[(uint16_t)(address >> 16)] + (address & 0xffff));
			if (trace_mem)
			{
#ifdef ENABLE_DO_TRACE
				if (do_trace)
					trace("Load %08x from %08x\n", value, address);
				else
#endif
				fprintf(log_file, "Load %08x from %08x\n", value, address);
			}
			return value;
#else
			return *(uint32_t*)(memory[(uint16_t)(address >> 16)] + (address & 0xffff));
#endif
		}
		uint32_t value;
		value  = memory[(uint16_t)(address >> 16)][(address & 0xffff)];
		address++;
		CHECK_MEM(address)
		value |= memory[(uint16_t)(address >> 16)][(address & 0xffff)] << 8;
		address++;
		CHECK_MEM(address)
		value |= memory[(uint16_t)(address >> 16)][(address & 0xffff)] << 16;
		address++;
		CHECK_MEM(address)
		value |= memory[(uint16_t)(address >> 16)][(address & 0xffff)] << 24;
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("Load %08x from %08x\n", value, address);
			else
#endif
			fprintf(log_file, "Load %08x from %08x\n", value, address);
		}
#endif
		return value;
	}
	
	void storeDWord(uint32_t address, uint32_t value)
	{
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("Store %08x at %08x\n", value, address);
			else
#endif
			fprintf(log_file, "Store %08x at %08x\n", value, address);
		}
#endif
#ifdef ENABLE_DO_TRACE
		if (address >= call_stack_top)
		{
			//printf("\nWarning: Assign %x to %x above %x\n", value, address, call_stack_top);
			//print_trace(log_file);
			//exit(1);
		}
#endif
		CHECK_MEM(address)
		if ((address & 0x3) == 0)
		{
			*(uint32_t*)(memory[(uint16_t)(address >> 16)] + (address & 0xffff)) = value;
		}
		else
		{
			memory[(uint16_t)(address >> 16)][(address & 0xffff)] = value & 0xff;
			address++;
			CHECK_MEM(address)
			value = value >> 8;
			memory[(uint16_t)(address >> 16)][(address & 0xffff)] = value & 0xff;
			address++;
			CHECK_MEM(address)
			value = value >> 8;
			memory[(uint16_t)(address >> 16)][(address & 0xffff)] = value & 0xff;
			address++;
			CHECK_MEM(address)
			value = value >> 8;
			memory[(uint16_t)(address >> 16)][(address & 0xffff)] = value & 0xff;
		}
	}

	void push(uint32_t value)
	{
		sp -= 4;
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("push %08x to %08x\n", value, sp);
			else
#endif
			fprintf(log_file, "push %08x to %08x\n", value, sp);
		}
		else { DO_TRACE("push %08x to %08x\n", value, sp); }
#endif
		if (sp < _sp_allocated)
		{
			_sp_allocated -= 0x10000;
			storeAllocByte(_sp_allocated, 0);
		}
		*(uint32_t*)(memory[(uint16_t)(sp >> 16)] + (sp & 0xffff)) = value;
	}
	
	void pushByte(byte value)
	{
		sp--;
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("push %02x to %08x\n", value, sp);
			else
#endif
			fprintf(log_file, "push %02x to %08x\n", value, sp);
		}
		else { DO_TRACE("push %08x to %08x\n", value, sp); }
#endif
		if (sp < _sp_allocated)
		{
			_sp_allocated -= 0x10000;
			storeAllocByte(_sp_allocated, 0);
		}
		memory[(uint16_t)(sp >> 16)][(sp & 0xffff)] = value;
	}
	
	uint32_t pop()
	{
		if (sp == 0L)
		{
			fprintf(log_file, "Stack underflow\n");
			exit(-1);
		}
		uint32_t value = *(uint32_t*)(memory[(uint16_t)(sp >> 16)] + (sp & 0xffff));
#ifdef TRACE_MEMORY
		if (trace_mem)
		{
#ifdef ENABLE_DO_TRACE
			if (do_trace)
				trace("pop %08x from %08x\n", value, sp);
			else
#endif
			fprintf(log_file, "pop %08x from %08x\n", value, sp);
		}
		else { DO_TRACE("pop %08x from %08x\n", value, sp); }
#endif
		sp += 4;
		
		return value;
	}
	
	bool increase_brk(uint32_t new_brk)
	{
		new_brk = (new_brk + 0xfff) & 0xfffff000;
		if (new_brk <= brk)
			return false;
		
		while (new_brk > brk)
			storeAllocByte(++brk, 0);
		return true;
	}

	char *name_for_function(uint32_t addr, int function_enter)
	{
		//fprintf(log_file, "name for function %08x %d\n", addr, function_enter);
		for (FunctionName *fn = functionNames; fn != 0; fn = fn->next)
			if (fn->addr == addr)
				return fn->name;
		static char buffer[100];
		if (function_enter == -1)
			sprintf(buffer, "%x", addr);
		else
			sprintf(buffer, "func%d", function_enter);
		return buffer;
	}
	
	char *name_for_addr(uint32_t addr)
	{
		for (FunctionName *fn = functionNames; fn != 0; fn = fn->next)
			if (fn->addr == addr)
				return fn->name;
		return 0;
	}
};

int Process::nr_of_processes = 0;

FunctionName* read_function_names()
{
	FunctionName *functionNames = 0;

	FILE *ffn = fopen("functions.txt", "r");
	if (ffn != 0)
	{
		char buffer[100];
		while (fgets(buffer, 100, ffn))
		{
			uint32_t a;
			sscanf(buffer, "%x", &a);
			char *s = buffer;
			while (*s != ' ' && *s != '\0')
				s++;
			while (*s == ' ')
				s++;
			int len = strlen(s);
			while (len > 0 && s[len-1] < ' ')
				len--;
			s[len] = '\0';
			// fprintf(log_file, "%08x %s\n", a, s);
			functionNames = new FunctionName(a, s, functionNames);  
		}
		fclose(ffn);
	}
	return functionNames;
}

void replace_with_val(FILE *fout, const char *code, const char *parm, uint32_t val, const char *fmt)
{
	int parm_len = strlen(parm);
	while (*code != '\0')
		if (*code == 'v' && strncmp(code, parm, parm_len) == 0)
		{
			fprintf(fout, fmt, val);
			code += parm_len;
		}
		else
		{
			fprintf(fout, "%c", *code);
			code++;
		}
	fprintf(fout, ";");
}

void generate_code(Process *process)
{
	int func_nr = 1;
	int label_nr = 1;
	uint32_t len = end_code - start_code;
	uint32_t index = start_pc - start_code;
	if (index < len && statements[index] != 0)
		statements[index]->function_enter = func_nr++; 
	for (uint32_t i = 0; i < end_code - start_code; i++)
	{
		Statement *stat = statements[i];
		if (stat != 0)
		{
			if (stat->kind == 'j' && stat->val32 != 0)
			{
				uint32_t index = stat->val32 - start_code;
				if (index < end_code && statements[index] != 0)
				{
					if (statements[index]->label_pos == 0)
						statements[index]->label_pos = label_nr++;
					if (index < i)
						statements[index]->is_loop = true;
				}
			}
			for (FunctionCall *func_call = stat->func_calls; func_call != 0; func_call = func_call->next)
			{
				uint32_t index = func_call->addr - start_code;
				if (index < end_code && statements[index] != 0)
					statements[index]->function_enter = -1;
			}
		}
	}
	for (uint32_t i = 0; i < end_code - start_code; i++)
	{
		Statement *stat = statements[i];
		if (stat != 0 && stat->function_enter == -1)
			stat->function_enter = func_nr++;
	}
	FILE *fout = fopen("program.cpp", "w");
	fprintf(fout, "#define INCLUDED\n");
	fprintf(fout, "#include \"Emulator.cpp\"\n");
	fprintf(fout, "#include \"program.h\"\n");
	fprintf(fout, "\n");
	fprintf(fout, "class MyProcessor : public Processor\n");
	fprintf(fout, "{\n");
	fprintf(fout, "  int indent;");
	fprintf(fout, "public:\n");
	fprintf(fout, "\tMyProcessor(Process *proc) : Processor(proc), indent(0), trace_func(false) {}\n");
	fprintf(fout, "\n");
	

	bool dyn_call = false;

	for (uint32_t i = 0; i < len; i++)
	{
		Statement *stat = statements[i];
		if (stat != 0 && stat->function_enter > 0)
		{
			for (uint32_t j = 0; j < len; j++)
				if (statements[j] != 0)
					statements[j]->gen_state = ' ';
			stat->gen_state = 'g';
					
			fprintf(fout, "\tvoid %s()\n\t{\n", process->name_for_function(start_code + i, stat->function_enter));
			fprintf(fout, "\t\tindent += 2; if (trace_func) fprintf(log_file, \"%%*.*s%s\\n\", indent, indent, \"\"); /* %x */\n", process->name_for_function(start_code + i, stat->function_enter), start_code + i);
			for (int labels_to_go = 1; labels_to_go > 0; )
			{
				bool output = false;
				for (uint32_t k = 0; k < len; k++)
				{
					stat = statements[k];
					if (stat == 0) continue;
					
					if (stat->gen_state == 'g')
					{
						stat->gen_state = 'D';
						labels_to_go--;
						output = true;
					}
					
					if (!output) continue;
					
					if (stat->label_pos > 0)
					{
						fprintf(fout, "\t\tlabel%d: _print_label(%d);%s", stat->label_pos, stat->label_pos, stat->is_loop ? " // loop" : "");
						if (stat->gen_state == 'g')
							labels_to_go--;
						char *label = process->name_for_addr(start_code + k);
						if (label != 0)
							fprintf(fout, " // %s", label);
						fprintf(fout, "\n");
						stat->gen_state = 'D';
					}

					if (stat->function_enter > 0 && stat->gen_state == ' ')
					{
						fprintf(fout, "\t\t\t\t\t\t\t%s();\n", process->name_for_function(start_code + k, stat->function_enter));
						fprintf(fout, "\t\tif (trace_func) _print_return();\n\t\tindent -= 2; return; \n");  
						output = false;
						continue;
					}

					//fprintf(fout, "\t\t/* %08x %c */ ", start_code + k, stat->kind);
					fprintf(fout, "\t\t");
					if (stat->kind == 'j')
					{
						uint32_t index = stat->val32 - start_code;
						if (index >= len || statements[index] == 0)
							fprintf(fout, "if (%s) { /* Jump is never taken. ERROR %08x %08x */ }", stat->code, stat->val32, index);
						else
						{
							fprintf(fout, "if (%s) goto label%d;", stat->code, statements[index]->label_pos);
							if (statements[index]->gen_state == ' ')
							{
								statements[index]->gen_state = 'g';
								labels_to_go++;
							}
							if (strcmp(stat->code, "true") == 0)
								output = false;
						}
					}
					else if (stat->kind == 'c')
					{
						if (stat->call_reg == 0)
						{
							if (stat->func_calls != 0 && stat->func_calls->next == 0)
							{
								uint32_t index = stat->func_calls->addr - start_code;
								if (index >= len || statements[index] == 0)
									fprintf(fout, "/* Jump ERROR %08x %08x */", stat->val32, index);
								else
									fprintf(fout, "%s;\t\t\t\t\t%s();", stat->code, process->name_for_function(stat->func_calls->addr, statements[index]->function_enter));
							}
							else
								fprintf(fout, "/* ERROR call */");
						}
						else
						{
							dyn_call = true;
							for (FunctionCall *func_call = stat->func_calls; func_call != 0; func_call = func_call->next)
							{
								uint32_t index = func_call->addr - start_code;
								if (index >= len || statements[index] == 0)
									fprintf(fout, "/* Jump ERROR %08x %08x */", stat->val32, index);
								else
									statements[index]->dyn_func = true;
							}
							fprintf(fout, "%s;\t\t\t\t\t_call_reg(_%s);", stat->code, stat->call_reg);
						}
					}
					else if (stat->kind == 'i')
					{
						switch (stat->val32)
						{
							case 0x01: fprintf(fout, "if (!int_exit()) exit(1);"); output = false; break;
							case 0x02: fprintf(fout, "int_fork();"); break;
							case 0x03: fprintf(fout, "int_read();"); break;
							case 0x04: fprintf(fout, "int_write();"); break;
							case 0x05: fprintf(fout, "int_open_file();"); break;
							case 0x06: fprintf(fout, "int_close_file();"); break;
							case 0x07: fprintf(fout, "int_wait_pid();"); break;
							case 0x0B: fprintf(fout, "if (!int_execve()) exit(1);"); break;
							case 0x0C: fprintf(fout, "int_chdir();"); break;
							case 0x0F: fprintf(fout, "int_chmod();"); break;
							case 0x13: fprintf(fout, "int_lseek();"); break;
							case 0x21: fprintf(fout, "int_access();"); break;
							case 0x2D: fprintf(fout, "int_sys_brk();"); break;
							default:   fprintf(fout, "// unhandled int80 %d", stat->val32); break;
						}
					}
					else if (stat->kind == 'r')
					{
						fprintf(fout, "%s; if (trace_func) _print_return();\n\t\tindent -= 2; return;", stat->code);
						output = false;
					}
					else
					{
						if (stat->kind == 'b')
							replace_with_val(fout, stat->code, "val8", stat->val8, "%u");
						else if (stat->kind == 'w')
							replace_with_val(fout, stat->code, "val16", stat->val16, "%u");
						else if (stat->kind == 'l')
						{
							replace_with_val(fout, stat->code, "val32", stat->val32, "0x%08x");
							if (start_code <= stat->val32 && stat->val32 < end_code)
							{
								uint32_t p = stat->val32;
								bool is_string = true;
								for(; p < end_code; p++)
								{
									byte ch = process->loadByte(p);
									if (ch == '\0')
										break;
									if (ch != '\n' && ch != '\t' && (ch < ' ' || ch > 127))
									{
										is_string = false;
										break;
									}
								}
								if (is_string && p > stat->val32)
								{
									fprintf(fout, "/* \"");
									for (uint32_t i = stat->val32; i < p; i++)
									{
										byte ch = process->loadByte(i);
										if (ch == '\n')
											fprintf(fout, "\\n");
										else if (ch == '\t')
											fprintf(fout, "\\t");
										else
											fprintf(fout, "%c", ch);
									}
									fprintf(fout, "\" */ ");
								}
							}
							char *name = process->name_for_addr(stat->val32);
							if (name != 0)
								fprintf(fout, "/* %s */", name);
						}
						else
							fprintf(fout, "%s;", stat->code);
					}
					fprintf(fout, "\n");
				}
			}
			fprintf(fout, "\t}\n\n");
		}
	}
	fprintf(fout, "private:\n");
	if (dyn_call)
	{
		fprintf(fout, "\tvoid _call_reg(uint32_t addr)\n");
		fprintf(fout, "\t{\n");
		for (uint32_t i = 0; i < len; i++)
		{
			Statement *stat = statements[i];
			if (stat != 0 && stat->dyn_func)
				fprintf(fout, "\t\tif (addr == 0x%08x) { %s(); return; }\n", start_code + i, process->name_for_function(start_code + i, stat->function_enter));
		}
		
		fprintf(fout, "\t\tprintf(\"Error, no function for 0x%%08x\\n\", addr); exit(1);\n"); 
		fprintf(fout, "\t}\n\n");
	}
	fprintf(fout, "\tuint64_t val64;\n");
	fprintf(fout, "\tuint32_t val32;\n");
	fprintf(fout, "\tuint16_t val16;\n");
	fprintf(fout, "\tbyte val8;\n");
	fprintf(fout, "\n");
	fprintf(fout, "\tbool trace_func;\n");
	fprintf(fout, "\n");
	fprintf(fout, "\tvoid _print_label(int n) { /* fprintf(log_file, \"%%*.*s- label%%d\\n\", indent, indent, \"\", n); */ }\n");
	fprintf(fout, "\tvoid _print_return()\n");
	fprintf(fout, "\t{\n");
	fprintf(fout, "\t\tprintf(\"%%*.*s=> %%d\", indent, indent, \"\", _eax);\n");
	fprintf(fout, "\t\tif (' ' <= _eax && _eax < 127) fprintf(log_file, \" '%%c'\", _eax);\n");
	fprintf(fout, "\t\tprintf(\"\\n\");\n");
	fprintf(fout, "\t}\n");
	fprintf(fout, "};\n");
	fprintf(fout, "\n");
	fprintf(fout, "int main(int argc, char *argv[])\n");
	fprintf(fout, "{\n");
	fprintf(fout, "\tProcess *main_process = mainProcess(argc, argv);\n");
	fprintf(fout, "\tif (main_process == 0)\n");
	fprintf(fout, "\t\treturn 0;\n");
	fprintf(fout, "\t\n");
	fprintf(fout, "\t\n");
	fprintf(fout, "\tMyProcessor processor(main_process);\n");
	fprintf(fout, "\tprocessor.%s();\n", process->name_for_function(start_code, 1));
	fprintf(fout, "	\n");
	fprintf(fout, "\treturn 0;\n");
	fprintf(fout, "}\n");
	
	fclose(fout);
}

void output_function_addresses(Process *process)
{
	FILE *fout = fopen("functions_out.txt", "w");
	
	for (uint32_t i = 0; i < end_code - start_code; i++)
	{
		Statement *stat = statements[i];
		if (stat != 0 && stat->function_enter > 0)
			fprintf(fout, "%08x %s\n", start_code + i, process->name_for_function(start_code + i, stat->function_enter));
	}
	fclose(fout);
}

Process *processes = 0;

Process *newProcess(Process *parent)
{
	static Process **ref_next_process = &processes;
	Process *process = new Process(parent);
	*ref_next_process = process;
	ref_next_process = &process->next;
	return process;
}

class Usage
{
public:
	bool as_input;
	bool as_output;
	bool as_exec;
	bool as_removed;
	
	File *file;
	Process *process;
	
	Usage *next_use;
	Usage *next_usage;
	
	Usage (File *_file, Process *_process) : as_input(false), as_output(false), as_exec(false), as_removed(false), file(_file), process(_process), next_use(0), next_usage(0)
	{
		Usage **ref_use = &process->uses;
		while (*ref_use != 0) ref_use = &(*ref_use)->next_use;
		*ref_use = this;
		Usage **ref_usage = &file->usages;
		while (*ref_usage != 0) ref_usage = &(*ref_usage)->next_usage;
		*ref_usage = this;
	}
	
	void is_input(FILE *fout)
	{
		as_input = true;
		file->used_as_input = true;
		if (fout != 0) { indent(fout); fprintf(fout, "%s file: %s\n", /*file->exists ? "Existing" :*/ file->produced ? "Produced" : "Input", file->name); }
	}
	void is_output(FILE *fout)
	{
		as_output = true;
		if (fout != 0) { indent(fout); fprintf(fout, "Output file: %s\n", file->name); }
		file->produced = true;
		file->produced_by = process;
		file->removed = false;
	}
	void is_exec(FILE *fout)
	{
		as_exec = true;
		if (fout != 0)
		{
			indent(fout); fprintf(fout, "Exec file: %s", file->name);
			//if (!file->exists && !file->produced)
			//	fprintf(fout, " Not existing nor produced");
			//if (file->exists)
			//	fprintf(fout, " Existing");
			if (file->produced)
				fprintf(fout, " Produced");
			fprintf(fout, "\n");
		}
	}
	void is_removed(FILE *fout)
	{
		as_removed = true;
		file->removed = true;
		indent(fout); fprintf(fout, "Deleted file: %s\n", file->name);
	}
};

void Process::finish()
{
	fprintf(log_file, "Close process\n");
	for (Usage *use = uses; use != 0; use = use->next_use)
		if (use->file->fh >= 0)
		{
			fprintf(log_file, " Close file %s\n", use->file->mappedPath());
			close(use->file->fh);
			use->file->fh = -1;
		}
	if (stat_file != 0) fflush(stat_file);
}

char cd_path[200] = "/";

void add_cd_path(char *filename)
{
	fprintf(log_file, "add_cd_path %s %s => ", cd_path, filename);
	if (filename[0] == '/')
	{
		fprintf(log_file, "%s\n", filename);
		return;
	}
	char buf[500];
	strcpy(buf, cd_path);
	int i = strlen(buf);
	while (i > 0 && buf[i-1] == '/')
		i--;
	char *f = filename;
	while (f[0] != '\0')
	{
		if (f[0] == '.' && (f[1] == '\0' || f[1] == '/'))
		{
			f++;
			while (f[0] == '/')
				f++;
		}
		else if (f[0] == '.' && f[1] == '.' && (f[2] == '\0' || f[2] == '/'))
		{
			f += 2;
			while (f[0] == '/')
				f++;
			while (i > 0 && buf[i-1] != '/')
				i--;
			while (i > 0 && buf[i-1] == '/')
				i--;
		}
		else
		{
			buf[i++] = '/';
			while (f[0] != '\0' && f[0] != '/')
				buf[i++] = *f++;
			while (f[0] == '/')
				f++;
		}
	}
	buf[i] = '\0';
	strcpy(filename, buf);
	fprintf(log_file, "%s\n", filename);
}

class ProgramFile
{
public:
	const char *fullname;
	byte *data = 0;
	uint32_t length;
	
	ProgramFile() : data(0), length(0) {}
	bool open(const char *name)
	{
		FILE *f = fopen(name, "r");
		if (f == 0)
		{
			return false;
		}
		int fh = fileno(f);
		length = lseek(fh, 0L, SEEK_END);
		lseek(fh, 0L, SEEK_SET);
		data = new byte[length];
		length = read(fh, data, length);
		fprintf(log_file, "Length %08x\n", length);
		//for (uint32_t i = 0; i < length; i++)
		//	DO_TRACE("%02X ", data[i]);
		//DO_TRACE("\n");
		fclose(f);
		return true;
	}
	
	unsigned short readShort(uint32_t &i)
	{
		unsigned short result = 0;
		result |= (unsigned short)data[i++];
		result |= (unsigned short)data[i++] << 8;
		return result;
	}
	
	byte readByte(uint32_t &i)
	{
		return data[i++];
	}
	
	uint32_t readLong(uint32_t &i)
	{
		uint32_t result = 0;
		result |= (uint32_t)data[i++];
		result |= (uint32_t)data[i++] << 8;
		result |= (uint32_t)data[i++] << 16;
		result |= (uint32_t)data[i++] << 24;
		DO_TRACE("readLong %08x %08x\n", i - 4, result);
		return result;
	}
};

bool loadELF(ProgramFile *file, Process *process)
{
	process->name = file->fullname;
	
	byte signature[24] = { 
		0x7F, 0x45, 0x4C, 0x46,
		0x01,
		0x01,
		0x01,
		0x03,
		0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x00, 
		0x03, 0x00,
		0x01, 0x00, 0x00, 0x00
	};
	uint32_t i = 0;
	for (; i < 24; i++)
		if (signature[i] != file->data[i])
		{
			fprintf(log_file, "ELF signature %2d %02X %02X\n", i, signature[i], file->data[i]);
			return false;
		}
	uint32_t pc = file->readLong(i);
	process->pc = pc;
	uint32_t phoff = file->readLong(i);
	if (phoff != 0x34)
	{
		fprintf(log_file, "Program header = %08x\n", phoff);
		return false;
	}
	uint32_t shoff = file->readLong(i);
	uint32_t e_flags = file->readLong(i);
	if (e_flags != 0)
	{
		fprintf(log_file, "e_flags = %08x\n", e_flags);
		return false;
	}
	unsigned short ehsize = file->readShort(i);
	if (ehsize != 0x34)
	{
		fprintf(log_file, "ehsize = %04x\n", ehsize);
		return false;
	}
	unsigned short phentsize = file->readShort(i);
	if (phentsize != 0x20)
	{
		fprintf(log_file, "phentsize = %04x\n", phentsize);
		return false;
	}
	unsigned short phnum = file->readShort(i);
	if (phnum != 1)
	{
		fprintf(log_file, "phnum = %04x\n", phnum);
		return false;
	}

	unsigned short shentsize = file->readShort(i);
	unsigned short shnum = file->readShort(i);
	unsigned short shstrndx = file->readShort(i);

	uint32_t from_file = ehsize + phentsize * phnum;
	
	for (int ih = 0; ih < phnum; ih++)
	{
		uint32_t ph_type = file->readLong(phoff);
		uint32_t ph_offset = file->readLong(phoff);
		uint32_t ph_vaddr = file->readLong(phoff);
		uint32_t ph_physaddr = file->readLong(phoff);
		uint32_t ph_filesz = file->readLong(phoff);
		uint32_t ph_memsz = file->readLong(phoff);
		uint32_t ph_flags = file->readLong(phoff);
		uint32_t ph_align = file->readLong(phoff);
		
		if (ph_type != 1)
		{
			fprintf(log_file, "ph_type = %08x\n", ph_type);
			return false;
		}
		if (ph_offset != 0)
		{
			fprintf(log_file, "ph_offset = %08x\n", ph_offset);
			return false;
		}
		if (ph_vaddr != ph_physaddr)
		{
			fprintf(log_file, "ph_vaddr %08x %08x\n", ph_vaddr, ph_physaddr);
			return false;
		}
		if (ph_filesz != ph_memsz)
		{
			fprintf(log_file, "ph_filesz %08x %08x\n", ph_filesz, ph_memsz);
			return false;
		}
		if (ph_flags != 7)
		{
			fprintf(log_file, "ph_flags = %08x\n", ph_flags);
			return false;
		}
		if (ph_align != 1)
		{
			fprintf(log_file, "ph_align = %08x\n", ph_align);
			return false;
		}
		
		if (shoff == 0)
		{
			fprintf(log_file, "ph_offset %x ph_vaddr %x\n", ph_offset, ph_vaddr);
			uint32_t to_mem = ph_vaddr + from_file;
			process->start_code = to_mem;
			for (uint32_t j = 0; j < ph_filesz; j++)
			{
				//fprintf(log_file, "Load byte from %08x: %02x ", from_file, file->data[from_file]);
				process->storeAllocByte(to_mem, file->data[from_file]);
				//fprintf(log_file, "Stored at %08x: %02x\n", to_mem, process->loadByte(to_mem)); 
				from_file++;
				to_mem++;
			}
			process->end_code = to_mem;
			process->brk = to_mem;
			process->increase_brk(to_mem);
		}
	}

	if (shoff != 0)
	{
		uint32_t shstrtab_addr = shoff + shstrndx * shentsize + 16;
		uint32_t shstrtab = file->readLong(shstrtab_addr);
		fprintf(log_file, "%08x\n", shstrtab);
		 
		for (uint32_t is = 0; is < shnum; is++)
		{
			uint32_t off = shoff + is * shentsize;
			fprintf(log_file, "Section info at %08x: ", off);
			uint32_t sh_name = file->readLong(off);
			uint32_t sh_type = file->readLong(off);
			uint32_t sh_flags = file->readLong(off);
			uint32_t sh_addr = file->readLong(off);
			uint32_t sh_offset = file->readLong(off);
			uint32_t sh_size = file->readLong(off);
			uint32_t sh_link = file->readLong(off);
			uint32_t sh_info = file->readLong(off);
			uint32_t sh_addralign = file->readLong(off);
			uint32_t sh_entsize = file->readLong(off);
			
			char name[11];
			{
				int i = 0;
				for (uint32_t s = shstrtab + sh_name;;)
				{
					byte ch = file->readByte(s);
					if (ch == 0)
						break;
					if (i < 10)
						name[i++] = ch;
				}
				name[i] = '\0';
			}
			fprintf(log_file, "%-10s ", name);
			
			if (sh_type == 0x0) // SHT_NULL
			{
				fprintf(log_file, "NULL     %08x %08x %08x %02x %08x %d %d %d\n",
					sh_addr, sh_offset, sh_size, sh_entsize, sh_flags, sh_link, sh_info, sh_addralign);
			}
			else if (sh_type == 0x1) // SHT_PROGBITS
			{
				fprintf(log_file, "PROGBITS %08x %08x %08x %02x %08x %d %d %d\n",
					sh_addr, sh_offset, sh_size, sh_entsize, sh_flags, sh_link, sh_info, sh_addralign);
				
				uint32_t section_end = file->length;
				if (is + 1 < shnum)
				{
					uint32_t next_addr = shoff + (is + 1) * shentsize + 16;
					section_end = file->readLong(next_addr);
				}			
				
				uint32_t to_mem = sh_addr;
				uint32_t from_file = sh_offset;
				process->start_code = to_mem;
				for (uint32_t from_file = sh_offset; from_file < section_end; from_file++)
				{
					//fprintf(log_file, "Load byte from %08x: %02x ", from_file, file->data[from_file]);
					process->storeAllocByte(to_mem, file->data[from_file]);
					//fprintf(log_file, "Stored at %08x: %02x\n", to_mem, process->loadByte(to_mem)); 
					to_mem++;
				}
				fprintf(log_file, "from_file: %08x to_mem: %08x\n", from_file, to_mem);
				process->end_code = to_mem;
				process->brk = to_mem;
				process->increase_brk(to_mem);
			}
			else if (sh_type == 0x2) // SHT_SYMTAB
			{
				fprintf(log_file, "SYMTAB   %08x %08x %08x %02x %08x %d %d %d\n",
					sh_addr, sh_offset, sh_size, sh_entsize, sh_flags, sh_link, sh_info, sh_addralign);
				
				uint32_t strtab_addr = shoff + sh_link * shentsize + 16;
				uint32_t strtab = file->readLong(strtab_addr);
				
				
				for (uint32_t i = 0; i < sh_info; i++)
				{
					uint32_t off = sh_offset + i * sh_entsize;
					uint32_t st_name = file->readLong(off);
					uint32_t st_value = file->readLong(off);
					uint32_t st_size = file->readLong(off);
					byte st_info = file->readByte(off);
					byte st_other = file->readByte(off);
					byte st_shndx1 = file->readByte(off);
					byte st_shndx2 = file->readByte(off);

					char symname[101];
					{
						int i = 0;
						for (uint32_t s = strtab + st_name;;)
						{
							byte ch = file->readByte(s);
							if (ch == 0)
								break;
							if (i < 100)
								symname[i++] = ch;
						}
						symname[i] = '\0';
					}

					//fprintf(log_file, "%08x %08x %08x %02x %02x %02x %02x '%s'\n",
					//	st_name, st_value, st_size, st_info, st_other, st_shndx1, st_shndx2, symname);
					fprintf(log_file, "%08x %s\n", st_value, symname);

					process->functionNames = new FunctionName(st_value, symname, process->functionNames);
				}
			}
			else if (sh_type == 0x3) // SHT_STRTAB
			{
				fprintf(log_file, "STRTAB   %08x %08x %08x %02x %08x %d %d %d\n",
					sh_addr, sh_offset, sh_size, sh_entsize, sh_flags, sh_link, sh_info, sh_addralign);
			}
			else
			{
				fprintf(log_file, "sh_type %08x is not supported\n", sh_type);
				return false;
			}
		}
	}
	
	return true;
}

#define SIGNEXT(X) ((uint32_t)(char)(X))

class Processor
{
public:
	Processor(Process *process) : _process(process) {}
	
	void run()
	{
		_pc = _process->pc;
		_eax = 0;
		_ebx = 0;
		_ecx = 0;
		_edx = 0;
		_esi = 0;
		_edi = 0;
		_ebp = 0;
		for (;;)
		{
			if (_pc < _process->start_code || _pc > _process->end_code)
			{
#ifdef ENABLE_DO_TRACE
				print_trace(log_file);
#endif
				gen_program();
				fprintf(log_file, "Program counter %08x outside code [%08x - %08x] %s\n", 
					_pc, _process->start_code, _process->end_code, _process->name);
				return;
			}
			
			START_INST(_pc);
			DO_TRACE("%08x ", _pc);
			byte opcode = getPC();
			
			uint64_t val64;
			uint32_t val32;
			uint16_t val16;
			byte val8;
			
			switch (opcode)
			{
				case 0x01:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC3:
							CODE(_ebx += _eax);
							DO_TRACE(" add_ebx,eax %08x\n", _ebx);
							break;
							
						case 0xC8:
							CODE(_eax += _ecx);
							DO_TRACE(" add_eax,ecx %08x\n", _eax);
							break;
							
						case 0xD8:
							CODE(_eax += _ebx);
							DO_TRACE(" add_eax,ebx %08x\n", _eax);
							break;
						
						case 0xE8:
							CODE(_eax += _ebp);
							DO_TRACE(" add_eax,ebp %08x\n", _eax);
							break;
							
						case 0xF0:
							CODE(_eax += _esi);
							DO_TRACE(" add_eax,esi %08x\n", _eax);
							break;
							
						case 0xF8:
							CODE(_eax += _edi);
							DO_TRACE(" add_eax,edi %08x\n", _eax);
							break;
							
						case 0xF9:
							CODE(_ecx += _edi);
							DO_TRACE(" add_ecx,edi %08x\n", _ecx);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x03:
					opcode = getPC();
					switch (opcode)
					{
						case 0x05:
							val32 = getLongPC();
							CODE_V32(_eax += _process->loadDWord(val32));
							DO_TRACE(" add_eax,memory[%08x]: %08x\n", val32, _eax);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x04:
				    val8 = getPC();
					CODE_V8(set_al((byte)(_eax & 0xFF) + val8));
					DO_TRACE(" addi8_al %02x %08x\n", val8, _eax);
					break;
					
				case 0x05:
					val32 = getLongPC();
					CODE_V32(_eax += val32);
					DO_TRACE(" add_eax %08x: %08x\n", val32, _eax);
					break;
				
				case 0x09:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD8:
							CODE(_eax = _eax | _ebx);
							DO_TRACE(" or_eax,ebx: %08x\n", _eax);
							break;

						default:
							unknownOpcode();
							return;
					}
					break;

				case 0x0F:
					opcode = getPC();
					switch (opcode)
					{
						case 0x84:
							val32 = getLongPC();
							DO_TRACE(" je %d\n", _flags);
							CODE_JUMP(_flags == 0, _pc + val32)
							break;
							
						case 0x85:
							val32 = getLongPC();
							DO_TRACE(" jne %d\n", _flags);
							CODE_JUMP(_flags != 0, _pc + val32)
							break;
							
						case 0x86:
							val32 = getLongPC();
							DO_TRACE(" jbe %d\n", _flags);
							CODE_JUMP(_flags <= 0, _pc + val32)
							break;
							
						case 0x8C:
							val32 = getLongPC();
							DO_TRACE(" jl %d\n", _flags);
							CODE_JUMP(_flags < 0, _pc + val32)
							break;
							
						case 0x8E:
							val32 = getLongPC();
							DO_TRACE(" jg %d\n", _flags);
							CODE_JUMP(_flags <= 0, _pc + val32)
							break;
							
						case 0x8F:
							val32 = getLongPC();
							DO_TRACE(" jg %d\n", _flags);
							CODE_JUMP(_flags > 0, _pc + val32)
							break;
						
						case 0xAF:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC3:
									CODE(_eax *= _ebx);
									DO_TRACE(" imul eax by ebx: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
								
						case 0xB6: // https://www.felixcloutier.com/x86/movzx
							opcode = getPC();
							switch (opcode)
							{
								case 0x00:
									CODE(_eax = _process->loadByte(_eax));
									DO_TRACE(" movzx (_eax), _eax: %08x\n", _eax);
									break;

								case 0xC0:
									CODE(_eax = _eax & 0xFF);
									DO_TRACE(" movzx _eax: %08x\n", _eax);
									break;
									
								case 0xC9:
									CODE(_ecx = _ecx & 0xFF);
									DO_TRACE(" movzx _cl: %08x\n", _ecx);
									break;
									
								case 0xDB:
									CODE(_ebx = _ebx & 0xFF);
									DO_TRACE(" movzx _ebx: %08x\n", _ebx);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0xBE:
							opcode = getPC();
							switch (opcode)
							{
								case 0x00:
									CODE(_eax = SIGNEXT(_process->loadByte(_eax)));
									DO_TRACE(" movsx_eax,BYTE_PTR_[eax]: %08x\n", _eax);
									break;
									
								case 0x1B:
									CODE(_ebx = SIGNEXT(_process->loadByte(_ebx)));
									DO_TRACE(" movsx_ebx,BYTE_PTR_[ebx]: %08x\n", _ebx);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x92:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags < 0 ? 1 : 0));
									DO_TRACE(" setb_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x93:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags >= 0 ? 1 : 0));
									DO_TRACE(" setae_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x94:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags == 0 ? 1 : 0));
									DO_TRACE(" sete_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x95:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags != 0 ? 1 : 0));
									DO_TRACE(" setne_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x96:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags <= 0 ? 1 : 0));
									DO_TRACE(" setbe_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x97:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags > 0 ? 1 : 0));
									DO_TRACE(" seta_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x9C:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags < 0 ? 1 : 0));
									DO_TRACE(" setl_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x9D:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags >= 0 ? 1 : 0));
									DO_TRACE(" setge_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x9E:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags <= 0 ? 1 : 0));
									DO_TRACE(" setle_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x9F:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC0:
									CODE(set_al(_flags > 0 ? 1 : 0));
									DO_TRACE(" setg_al: %08x\n", _eax);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x21:
					opcode = getPC();
					switch(opcode)
					{
						case 0xD8:
							CODE(_eax = _eax & _ebx);
							DO_TRACE(" and_ebx:%08x to eax: %08xc\n", _ebx, _eax);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x25:
					val32 = getLongPC();
					CODE_V32(_eax = _eax & val32);
					DO_TRACE(" and eax imm %08x %08x\n", val32, _eax);
					break;
					
				case 0x29:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC3:
							CODE(_ebx -= _eax);
							DO_TRACE(" sub_eax:%08x from ebx: %08xc\n", _eax, _ebx);
							break;
						
						case 0xD0:
							CODE(_eax -= _edx);
							DO_TRACE(" sub_edx:%08x from eax: %08xc\n", _edx, _eax);
							break;
						
						case 0xF8:
							CODE(_eax -= _edi);
							DO_TRACE(" sub_edi:%08x from eax: %08xc\n", _edi, _eax);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0x2C:
					val8 = getPC();
					DO_TRACE(" sub_al, %d\n", opcode);
					CODE_V8(set_al((byte)(_eax & 0xFF) - val8));
					break;
					 
				case 0x31:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC0:
							CODE(_eax = 0L);
							DO_TRACE(" xor_eax,eax\n");
							break;
						
						case 0xC9:
							CODE(_ecx = 0L);
							DO_TRACE(" xor_ecx,ecx\n");
							break;
						
						case 0xD2:
							CODE(_edx = 0L);
							DO_TRACE(" xor_edx,edx\n");
							break;

						case 0xD8:
							CODE(_eax = _eax ^ _ebx);
							DO_TRACE(" xor_eax,ebx\n");
							break;

						case 0xDB:
							CODE(_ebx = 0L);
							DO_TRACE(" xor_ebx,ebx\n");
							break;

						case 0xED:
							CODE(_ebp = 0L);
							DO_TRACE(" xor_ebp,ebp\n");
							break;

						case 0xFF:
							CODE(_edi = 0L);
							DO_TRACE(" xor_edi,edi\n");
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x38:
					opcode = getPC();
					switch(opcode)
					{
						case 0xD8:
							CODE(_flags = SIGNEXT(_eax & 0xFF) - SIGNEXT(_ebx & 0xFF));
							DO_TRACE(" cmp_al_bl %08x %08x %d\n", _eax, _ebx, _flags);
							break;

						default:
							unknownOpcode();
							return;
					}
					break;

				case 0x39: // CMP r/m32 r32
					opcode = getPC();
					switch(opcode)
					{
						case 0xC3: // 11 000 EAX 011 EBX 
							CODE(_flags = _ebx - _eax);
							DO_TRACE(" cmp_ebx,eax\n");
							break;

						case 0xC8: //11 001 ECX 000 EAX CMP_EAX_ECX
							CODE(_flags = _eax - _ecx);
							DO_TRACE(" cmp_eax,ecx\n");
							break;

						case 0xCB: // 11 001 ECX 011 EBX CMP_ECX_EBX
							CODE(_flags = _ebx - _ecx);
							DO_TRACE(" cmp_ecx,ebx\n");
							break;

						case 0xD3: // 11 010 EDX 011 EBX CMP_EBX_EDX
							CODE(_flags = _ebx - _edx);
							DO_TRACE(" cmp_ebx,edx\n");
							break;

						case 0xD8: // 11 011 EBX 000 EAX CMP_EAX_EBX
							CODE(_flags = _eax - _ebx);
							DO_TRACE(" cmp_eax,ebx\n");
							break;

						case 0xD9: // 11 011 EBX 001 ECX CMP_EBX_ECX
							CODE(_flags = _ecx - _ebx);
							DO_TRACE(" cmp_ebx,ecx\n");
							break;

						case 0xFE: // 11 111 EDI 110 ESI CMP_EDI_ESI
							CODE(_flags = _esi - _edi);
							DO_TRACE(" cmp_edi,esi\n");
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
						
				case 0x3C:
					val8 = getPC();
					CODE_V8(_flags = SIGNEXT(_eax & 0xFF) - SIGNEXT(val8));
					DO_TRACE(" cmp_al, %d = %d\n", val8, _flags);
					break;
					
				case 0x3D:
					val32 = getLongPC();
					CODE_V32(_flags = _eax - val32);
					DO_TRACE(" cmp_eax, %d = %d\n", val32, _flags);
					break;
					
				case 0x4D:
					CODE(_ebp--);
					DO_TRACE(" dec ebp: %08x\n", _ebp);
					break;
					
				case 0x50: CODE(_process->push(_eax)); DO_TRACE(" push_eax\n"); break;
				case 0x51: CODE(_process->push(_ecx)); DO_TRACE(" push_ecx\n"); break;
				case 0x52: CODE(_process->push(_edx)); DO_TRACE(" push_edx\n"); break;
				case 0x53: CODE(_process->push(_ebx)); DO_TRACE(" push_ebx\n"); break;
				case 0x55: CODE(_process->push(_ebp)); DO_TRACE(" push_ebp\n"); break;
				case 0x56: CODE(_process->push(_esi)); DO_TRACE(" push_esi\n"); break;
				case 0x57: CODE(_process->push(_edi)); DO_TRACE(" push_edi\n"); break;
					
				case 0x58: CODE(_eax = _process->pop()); DO_TRACE(" pop_eax: %08x\n", _eax); break;
				case 0x59: CODE(_ecx = _process->pop()); DO_TRACE(" pop_ecx: %08x\n", _ecx); break;
				case 0x5A: CODE(_edx = _process->pop()); DO_TRACE(" pop_edx: %08x\n", _ebx); break;
				case 0x5B: CODE(_ebx = _process->pop()); DO_TRACE(" pop_ebx: %08x\n", _ebx); break;
				case 0x5D: CODE(_ebp = _process->pop()); DO_TRACE(" pop_ebp: %08x\n", _ebp); break;
				case 0x5E: CODE(_esi = _process->pop()); DO_TRACE(" pop_esi: %08x\n", _esi); break;
				case 0x5F: CODE(_edi = _process->pop()); DO_TRACE(" pop_edi: %08x\n", _edi); break;
					
				case 0x6A:
					val8 = getPC();
					CODE_V8(_process->push(SIGNEXT(val8)));
					DO_TRACE(" push %02x\n", val8);
					break;
				
				case 0x6B:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							val8 = getPC();
							CODE_V8(_eax *= SIGNEXT(val8)); // Or? set_cx(getShortPC());
							DO_TRACE(" imuli8 eax %02x: %08x\n", opcode, _eax);
							break;
							
						case 0xED:
							val8 = getPC();
							CODE_V8(_ebp *= SIGNEXT(val8)); // Or? set_cx(getShortPC());
							DO_TRACE(" imuli8 ebp %02x: %08x\n", val8, _ebp);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x66:
					opcode = getPC();
					switch (opcode)
					{
						case 0xB9:
						    val16 = getShortPC();
							CODE_V16(_ecx = val16); // Or? set_cx(getShortPC());
							break;
							
						case 0xBA:
						    val16 = getShortPC();
							CODE_V16(_edx = val16); // Or? set_dx(getShortPC());
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x74:
					opcode = getPC();
					DO_TRACE(" je %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags == 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x75:
					opcode = getPC();
					DO_TRACE(" jne %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags != 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x76:
					opcode = getPC();
					DO_TRACE(" jbe8 %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags <= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7C:
					opcode = getPC();
					DO_TRACE(" jl %02X  flags = %d\n", opcode, _flags);
					CODE_JUMP(_flags < 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7D:
					opcode = getPC();
					DO_TRACE(" jge %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags >= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7E:
					opcode = getPC();
					DO_TRACE(" jle %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags <= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7F:
					opcode = getPC();
					DO_TRACE(" jg %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags > 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x81:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							val32 = getLongPC();
							CODE_V32(_eax += val32);
							DO_TRACE(" add_eax %08x: %08x\n", val32, _eax);
							break;

						case 0xC3:
							val32 = getLongPC();
							CODE_V32(_ebx += val32);
							DO_TRACE(" add_ebx %08x: %08x\n", val32, _ebx);
							break;

						case 0xC5:
							val32 = getLongPC();
							CODE_V32(_ebp += val32);
							DO_TRACE(" add_ebp %08x: %08x\n", val32, _ebp);
							break;
								
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x83:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							val8 = getPC();
							CODE_V8(_eax += SIGNEXT(val8));
							DO_TRACE(" add_eax %02x %08x\n", val8, _eax);
							break;
							
						case 0xC1:
							val8 = getPC();
							CODE_V8(_ecx += SIGNEXT(val8));
							DO_TRACE(" add_ecx %02x %08x\n", val8, _ecx);
							break;
							
						case 0xC2:
							val8 = getPC();
							CODE_V8(_edx += SIGNEXT(val8));
							DO_TRACE(" add_edx %02x %08x\n", val8, _edx);
							break;
						
						case 0xC3:
							val8 = getPC();
							CODE_V8(_ebx += SIGNEXT(val8));
							DO_TRACE(" add_ebx %02x %08x\n", val8, _ebx);
							break;
						
						case 0xC4:
							val8 = getPC();
							CODE_V8(_process->sp += SIGNEXT(val8));
							DO_TRACE(" add_esp %02x %08x\n", val8, _process->sp);
							break;
						
						case 0xC5:
							val8 = getPC();
							CODE_V8(_ebp += SIGNEXT(val8));
							DO_TRACE(" add_ebp %02x: %08x\n", val8, _ebp);
							break;
						
						case 0xC6:
							val8 = getPC();
							CODE_V8(_esi += SIGNEXT(val8));
							DO_TRACE(" add_esi %02x: %08x\n", val8, _esi);
							break;
						
						case 0xC7:
							val8 = getPC();
							CODE_V8(_edi += SIGNEXT(val8));
							DO_TRACE(" add_edi %02x: %08x\n", val8, _edi);
							break;
						
						case 0xE0: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_eax = _eax & SIGNEXT(val8));
							DO_TRACE(" sub _eax %02x: %08x\n", val8, _eax);
							break;
								
						case 0xE8: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_eax -= SIGNEXT(val8));
							DO_TRACE(" sub _eax %02x: %08x\n", val8, _eax);
							break;
								
						case 0xE9: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_ecx -= SIGNEXT(val8));
							DO_TRACE(" sub _ecx %02x: %08x\n", val8, _ecx);
							break;
								
						case 0xEE:
							val8 = getPC();
							CODE_V8(_esi -= SIGNEXT(val8));
							DO_TRACE(" sub _esi %02x: %08x\n", val8, _esi);
							break;
								
						case 0xF8: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _eax - SIGNEXT(val8));
							DO_TRACE(" cmp _eax %02x: %08x\n", val8, _flags);
							break;
								
						case 0xF9: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ecx - SIGNEXT(val8));
							DO_TRACE(" cmp _ecx %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFA: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _edx - SIGNEXT(val8));
							DO_TRACE(" cmp _edx %02x: %08x\n", val8, _flags);
							break;

						case 0xFB: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ebx - SIGNEXT(val8));
							DO_TRACE(" cmp _ebx %02x: %08x\n", val8, _flags);
							break;

						case 0xFD: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ebp - SIGNEXT(val8));
							DO_TRACE(" cmp _ebp %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFE: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _esi - SIGNEXT(val8));
							DO_TRACE(" cmp _esi %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFF: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _edi - SIGNEXT(val8));
							DO_TRACE(" cmp _edi %02x: %08x\n", val8, _flags);
							break;
								
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x85:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							CODE(_flags = (int32_t)_eax);
							DO_TRACE(" test_eax,eax %08x %d\n", _eax, _flags);
							break;
							
						case 0xDB:
							CODE(_flags = (int32_t)_ebx);
							DO_TRACE(" test_ebx,ebx %08x %d\n", _ebx, _flags);
							break;
							
						case 0xED:
							CODE(_flags = (int32_t)_ebp);
							DO_TRACE(" test_ebp,ebp %08x %d\n", _ebp, _flags);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x88:
					opcode = getPC();
					switch (opcode)
					{
						case 0x01:
							CODE(_process->storeByte(_ecx, (byte)_eax));
							DO_TRACE(" mov_[ecx:%08x],al %02x\n", _ecx, (byte)_eax);
							break;
					
						case 0x03:
							CODE(_process->storeByte(_ebx, (byte)_eax));
							DO_TRACE(" mov_[ebx:%08x],al %02x\n", _ebx, (byte)_eax);
							break;
					
						case 0x06:
							CODE(_process->storeByte(_esi, (byte)_eax));
							DO_TRACE(" mov_[esi:%08x],al %02x\n", _esi, (byte)_eax);
							break;
					
						case 0x0A:
							CODE(_process->storeByte(_edx, (byte)_ecx));
							DO_TRACE(" mov_[edx:%08x],cl %02x\n", _edx, (byte)_ecx);
							break;
					
						case 0x19:
							CODE(_process->storeByte(_ecx, (byte)_ebx));
							DO_TRACE(" mov_[ecx:%08x],bl %02x\n", _ecx, (byte)_ebx);
							break;
					
						default:
							unknownOpcode();
							return;
					}
					break;

				case 0x89:
					opcode = getPC();
					switch (opcode)
					{
						case 0x01:
							CODE(_process->storeDWord(_ecx, _eax));
							DO_TRACE(" mov_[ecx:%08x],eax %08x\n", _ecx, _eax);
							break;
						
						case 0x02:
							CODE(_process->storeDWord(_edx, _eax));
							DO_TRACE(" mov_[edx:%08x],eax %08x\n", _edx, _eax);
							break;
						
						case 0x03:
							CODE(_process->storeDWord(_ebx, _eax));
							DO_TRACE(" mov_[ebx:%08x],eax %08x\n", _ebx, _eax);
							break;
						
						case 0x08:
							CODE(_process->storeDWord(_eax, _ecx));
							DO_TRACE(" mov_[eax:%08x],ecx %08x\n", _eax, _ecx);
							break;
						
						case 0x0B:
							CODE(_process->storeDWord(_ebx, _ecx));
							DO_TRACE(" mov_[ebx:%08x],ecx %08x\n", _ebx, _ecx);
							break;
						
						case 0x0D:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _ecx));
							DO_TRACE(" mov_ecx: %08x to memory[%08x]\n", _ecx, val32);
							break;
						
						case 0x15:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _edx));
							DO_TRACE(" mov_edx: %08x to memory[%08x]\n", _edx, val32);
							break;
						
						case 0x18:
							CODE(_process->storeDWord(_eax, _ebx));
							DO_TRACE(" mov_[eax:%08x],ebx %08x\n", _eax, _ebx);
							break;
						
						case 0x1D:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _ebx));
							DO_TRACE(" mov_ebx: %08x to memory[%08x]\n", _ebx, val32);
							break;
						
						case 0x2A:
							CODE(_process->storeDWord(_edx, _ebp));
							DO_TRACE(" mov_ebp: %08x to memory[edx:%08x]\n", _ebp, _edx);
							break;

						case 0x30:
							CODE(_process->storeDWord(_eax, _esi));
							DO_TRACE(" mov_esi: %08x to memory[eax:%08x]\n", _esi, _eax);
							break;
						
						case 0x38:
							CODE(_process->storeDWord(_eax, _edi));
							DO_TRACE(" mov_edi: %08x to memory[eax:%08x]\n", _edi, _eax);
							break;
						
						case 0x41:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ecx + SIGNEXT(val8), _eax));
							DO_TRACE(" mov_eax: %08x to memory[ecx:%08x + %02x]\n", _eax, _ecx, val8);
							break;
						
						case 0x42:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + SIGNEXT(val8), _eax));
							DO_TRACE(" mov_eax: %08x to memory[edx:%08x + %02x]\n", _eax, _edx, val8);
							break;
						
						case 0x45:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + SIGNEXT(val8), _eax));
							DO_TRACE(" mov_eax: %08x to memory[ebp:%08x + %02x]\n", _eax, _ebp, val8);
							break;
						
						case 0x48:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + SIGNEXT(val8), _ecx));
							DO_TRACE(" mov_ecx: %08x to memory[eax:%08x + %02x]\n", _ecx, _eax, val8);
							break;
						
						case 0x4A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + SIGNEXT(val8), _ecx));
							DO_TRACE(" mov_ecx: %08x to memory[edx:%08x + %02x]\n", _ecx, _edx, val8);
							break;
						
						case 0x50:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + SIGNEXT(val8), _edx));
							DO_TRACE(" mov_edx: %08x to memory[eax:%08x + %02x]\n", _edx, _eax, val8);
							break;
						
						case 0x55:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + SIGNEXT(val8), _edx));
							DO_TRACE(" mov_edx: %08x to memory[ebp:%08x + %02x]\n", _edx, _ebp, val8);
							break;
						
						case 0x58:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + SIGNEXT(val8), _ebx));
							DO_TRACE(" mov_ebx: %08x to memory[eax:%08x + %02x]\n", _ebx, _eax, val8);
							break;
						
						case 0x59:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ecx + SIGNEXT(val8), _ebx));
							DO_TRACE(" mov_ebx: %08x to memory[ecx:%08x + %02x]\n", _ebx, _ecx, val8);
							break;
						
						case 0x5A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + SIGNEXT(val8), _ebx));
							DO_TRACE(" mov_ebx: %08x to memory[edx:%08x + %02x]\n", _ebx, _edx, val8);
							break;
						
						case 0x6A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + SIGNEXT(val8), _ebp));
							DO_TRACE(" mov_ebp: %08x to memory[edx:%08x + %02x]\n", _ebp, _edx, val8);
							break;
						
						case 0x6E:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_esi + SIGNEXT(val8), _ebp));
							DO_TRACE(" mov_ebp: %08x to memory[esi:%08x + %02x]\n", _ebp, _esi, val8);
							break;
						
						case 0x72:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + SIGNEXT(val8), _esi));
							DO_TRACE(" mov_esi: %08x to memory[edx:%08x + %02x]\n", _esi, _edx, val8);
							break;
						
						case 0x75:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + SIGNEXT(val8), _esi));
							DO_TRACE(" mov_esi: %08x to memory[ebp:%08x + %02x]\n", _esi, _ebp, val8);
							break;
						
						case 0x78:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + SIGNEXT(val8), _edi));
							DO_TRACE(" mov_edi: %08x to memory[eax:%08x + %02x]\n", _edi, _eax, val8);
							break;
						
						case 0xC1: CODE(_ecx = _eax); DO_TRACE(" mov_ecx,eax = %08x\n", _ecx); break;
						case 0xC2: CODE(_edx = _eax); DO_TRACE(" mov_edx,eax = %08x\n", _edx); break;
						case 0xC3: CODE(_ebx = _eax); DO_TRACE(" mov_ebx,eax = %08x\n", _ebx); break;
						case 0xC5: CODE(_ebp = _eax); DO_TRACE(" mov_ebp,eax = %08x\n", _ebp); break;
						case 0xC6: CODE(_esi = _eax); DO_TRACE(" mov_esi,eax = %08x\n", _esi); break;
						case 0xC7: CODE(_edi = _eax); DO_TRACE(" mov_edi,eax = %08x\n", _edi); break;
						
						case 0xC8: CODE(_eax = _ecx); DO_TRACE(" mov_aex,ecx = %08x\n", _eax); break;
						case 0xCB: CODE(_ebx = _ecx); DO_TRACE(" mov_abx,ecx = %08x\n", _ebx); break;
						
						case 0xD0: CODE(_eax = _edx); DO_TRACE(" mov_eax,edx = %08x\n", _eax); break;
						case 0xD3: CODE(_ebx = _edx); DO_TRACE(" mov_ebx,edx = %08x\n", _ebx); break;
						case 0xD5: CODE(_ebp = _edx); DO_TRACE(" mov_ebp,edx = %08x\n", _ebp); break;
						
						case 0xD8: CODE(_eax = _ebx); DO_TRACE(" mov_eax,ebx = %08x\n", _eax); break;
						case 0xD9: CODE(_ecx = _ebx); DO_TRACE(" mov_ecx,ebx = %08x\n", _ecx); break;
						case 0xDA: CODE(_edx = _ebx); DO_TRACE(" mov_edx,ebx = %08x\n", _edx); break;
						case 0xDD: CODE(_ebp = _ebx); DO_TRACE(" mov_ebp,ebx = %08x\n", _ebp); break;
						case 0xDF: CODE(_edi = _ebx); DO_TRACE(" mov_edi,ebx = %08x\n", _edi); break;
						
						case 0xE1: CODE(_ecx = _process->sp); DO_TRACE(" mov_ecx,esp = %08x\n", _ecx); break;
						case 0xE5: CODE(_ebp = _process->sp); DO_TRACE(" mov_ebp,esp = %08x\n", _ebp); break;
						case 0xE7: CODE(_edi = _process->sp); DO_TRACE(" mov_edi,esp = %08x\n", _edi); break;
						
						case 0xE8: CODE(_eax = _ebp); DO_TRACE(" mov_eax,ebp = %08x\n", _eax); break;
						case 0xEA: CODE(_edx = _ebp); DO_TRACE(" mov_edx,ebp = %08x\n", _edx); break;
						case 0xEB: CODE(_ebx = _ebp); DO_TRACE(" mov_ebx,ebp = %08x\n", _ebx); break;
						
						case 0xF0: CODE(_eax = _esi); DO_TRACE(" mov_eax,esi = %08x\n", _eax); break;
						case 0xF3: CODE(_ebx = _esi); DO_TRACE(" mov_ebx,esi = %08x\n", _ebx); break;
						case 0xF7: CODE(_edi = _esi); DO_TRACE(" mov_edi,esi = %08x\n", _edi); break;
						
						case 0xF8: CODE(_eax = _edi); DO_TRACE(" mov_eax,edi = %08x\n", _eax); break;
						case 0xF9: CODE(_ecx = _edi); DO_TRACE(" mov_ecx,edi = %08x\n", _ecx); break;
						case 0xFA: CODE(_edx = _edi); DO_TRACE(" mov_edx,edi = %08x\n", _edx); break;
						case 0xFB: CODE(_flags = _ebx = _edi); DO_TRACE(" text ebx,ebx %d\n", _flags); break;
						case 0xFD: CODE(_ebp = _edi); DO_TRACE(" mov_ebp,edi = %08x\n", _ebp); break;
						case 0xFE: CODE(_esi = _edi); DO_TRACE(" mov_esi,edi = %08x\n", _esi); break;
						//case 0xFB: _flags = _ebx; DO_TRACE(" text ebx,ebx %d\n", _flags); break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
			
				case 0x8A:
					opcode = getPC();
					switch (opcode)
					{
						case 0x00:
							CODE(set_al(_process->loadByte(_eax)));
							DO_TRACE(" mov_al,[eax] %08x\n", _eax);
							break; 
							
						case 0x01:
							CODE(set_al(_process->loadByte(_ecx)));
							DO_TRACE(" mov_al,[ecx:%08x] %08x\n", _ecx, _eax);
							break; 
							
						case 0x02:
							CODE(set_al(_process->loadByte(_edx)));
							DO_TRACE(" mov_al,[edx:%08x] %08x\n", _edx, _eax);
							break; 
							
						case 0x03:
							CODE(set_al(_process->loadByte(_ebx)));
							DO_TRACE(" mov_al,[ebx:%08x] %08x\n", _ebx, _eax);
							break;
							
						case 0x04:
							opcode = getPC();
							switch (opcode)
							{
								case 0x0B:
									CODE(set_al(_process->loadByte(_ebx + _ecx)));
									DO_TRACE(" mov_al,[ebx:%08x + ecx:%08x] %08x\n", _ebx, _eax, _ecx);
									break;
								
								default:
									unknownOpcode();
									return;
							}
							break;
							
						case 0x08:
							CODE(set_cl(_process->loadByte(_eax)));
							DO_TRACE(" mov_cl,[ebx:%08x] %08x\n", _eax, _ecx);
							break; 
							
						case 0x0B:
							CODE(set_cl(_process->loadByte(_ebx)));
							DO_TRACE(" mov_cl,[ebx:%08x] %08x\n", _ebx, _ecx);
							break; 
							
						case 0x18:
							CODE(set_bl(_process->loadByte(_eax)));
							DO_TRACE(" mov_bl,[eax:%08x] %08x\n", _eax, _ebx);
							break; 
							
						case 0x19:
							CODE(set_bl(_process->loadByte(_ecx)));
							DO_TRACE(" mov_bl,[ecx:%08x] %08x\n", _ecx, _ebx);
							break; 
							
						case 0x1A:
							CODE(set_bl(_process->loadByte(_edx)));
							DO_TRACE(" mov_bl,[edx:%08x] %08x\n", _edx, _ebx);
							break; 
							
						case 0x1B:
							CODE(set_bl(_process->loadByte(_ebx)));
							DO_TRACE(" mov_bl,[ebx] %08x\n", _ebx);
							break; 
							
						case 0x4B:
							val8 = getPC();
							CODE_V8(set_cl(_process->loadByte(_ebx + SIGNEXT(val8))));
							DO_TRACE(" mov_bl,[edx:%08x + %02x] %08x\n", _ebx, val8, _ecx);
							break; 
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x8B:
					opcode = getPC();
					switch (opcode)
					{
						case 0x00:
							CODE(_eax = _process->loadDWord(_eax));
							DO_TRACE(" mov_eax,[eax] %08x\n", _eax);
							break; 
						
						case 0x01:
							CODE(_eax = _process->loadDWord(_ecx));
							DO_TRACE(" mov_eax,[ebx:%08x] %08x\n", _ecx, _eax);
							break; 
						
						case 0x03:
							CODE(_eax = _process->loadDWord(_ebx));
							DO_TRACE(" mov_eax,[ebx:%08x] %08x\n", _ebx, _eax);
							break; 
						
						case 0x09:
							CODE(_ecx = _process->loadDWord(_ecx));
							DO_TRACE(" mov_eax,[ecx] %08x\n", _ecx);
							break; 
						
						case 0x0B:
							CODE(_ecx = _process->loadDWord(_ebx));
							DO_TRACE(" mov_ecx,[ebx:%08x] %08x\n", _ebx, _ecx);
							break; 

						case 0x0D:
							val32 = getLongPC();
							CODE_V32(_ecx = _process->loadDWord(val32));
							DO_TRACE(" mov_ecx: %08x from memory[%08x]\n", _ecx, val32);
							break;
							
						case 0x12:
							CODE(_edx = _process->loadDWord(_edx));
							DO_TRACE(" mov_edx,[edx] %08x\n", _edx);
							break; 
						
						case 0x1B:
							CODE(_ebx = _process->loadDWord(_ebx));
							DO_TRACE(" mov_ebs,[ebx] %08x\n", _ebx);
							break;
					
						case 0x1D:
							val32 = getLongPC();
							CODE_V32(_ebx = _process->loadDWord(val32));
							DO_TRACE(" mov_ebx: %08x from memory[%08x]\n", _ebx, val32);
							break;
							
						case 0x36:
							CODE(_esi = _process->loadDWord(_esi));
							DO_TRACE(" mov_esi,[esi] %08x\n", _esi);
							break; 

						case 0x40:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_eax + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[eax + %02x]\n", _eax, val8);
							break;
							
						case 0x41:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ecx + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[ecx:%08x + %02x]\n", _eax, _ecx, val8);
							break;
							
						case 0x42:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_edx + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[edx:%08x + %02x]\n", _eax, _edx, val8);
							break;
							
						case 0x43:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ebx + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[ebx:%08x + %02x]\n", _eax, _ebx, val8);
							break;
							
						case 0x45:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[%08x + %02x]\n", _eax, _ebp, val8);
							break;
							
						case 0x46:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_esi + SIGNEXT(val8)));
							DO_TRACE(" mov_eax: %08x from memory[%08x + %02x]\n", _eax, _esi, val8);
							break;
							
						case 0x48:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_eax + SIGNEXT(val8)));
							DO_TRACE(" mov_ecx: %08x from memory[%08x + %02x]\n", _ecx, _eax, val8);
							break;
							
						case 0x49:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_ecx + SIGNEXT(val8)));
							DO_TRACE(" mov_ecx: %08x from memory[ecx + %02x]\n", _ecx, val8);
							break;
							
						case 0x4A:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_edx + SIGNEXT(val8)));
							DO_TRACE(" mov_ecx: %08x from memory[%08x + %02x]\n", _ecx, _edx, val8);
							break;
							
						case 0x4D:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_ecx: %08x from memory[%08x + %02x]\n", _ecx, _ebp, val8);
							break;
							
						case 0x52:
							val8 = getPC();
							CODE_V8(_edx = _process->loadDWord(_edx + SIGNEXT(val8)));
							DO_TRACE(" mov_edx: %08x from memory[edx + %02x]\n", _edx, val8);
							break;
							
						case 0x55:
							val8 = getPC();
							CODE_V8(_edx = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_edx: %08x from memory[%08x + %02x]\n", _edx, _ebp, val8);
							break;
							
						case 0x56:
							val8 = getPC();
							CODE_V8(_edx = _process->loadDWord(_esi + SIGNEXT(val8)));
							DO_TRACE(" mov_edx: %08x from memory[%08x + %02x]\n", _edx, _esi, val8);
							break;
							
						case 0x58:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_eax + SIGNEXT(val8)));
							DO_TRACE(" mov_ebx: %08x from memory[%08x + %02x]\n", _ebx, _eax, val8);
							break;
							
						case 0x59:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_ecx + SIGNEXT(val8)));
							DO_TRACE(" mov_ebx: %08x from memory[%08x + %02x]\n", _ebx, _ecx, val8);
							break;
							
						case 0x5B:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_ebx + SIGNEXT(val8)));
							DO_TRACE(" mov_ebx: %08x from memory[ebx + %02x]\n", _ebx, val8);
							break;
							
						case 0x5D:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_ebx: %08x from memory[ebp + %02x]\n", _ebx, val8);
							break;
							
						case 0x6D:
							val8 = getPC();
							CODE_V8(_ebp = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_ebp: %08x from memory[ebp + %02x]\n", _ebp, val8);
							break;
							
						case 0x75:
							val8 = getPC();
							CODE_V8(_esi = _process->loadDWord(_ebp + SIGNEXT(val8)));
							DO_TRACE(" mov_esi: %08x from memory[%08x + %02x]\n", _esi, _ebp, val8);
							break;
						
						case 0x7A:
							val8 = getPC();
							CODE_V8(_edi = _process->loadDWord(_edx + SIGNEXT(val8)));
							DO_TRACE(" mov_edi: %08x from memory[%08x + %02x]\n", _edi, _edx, val8);
							break;
						
						case 0x84:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_eax = _process->loadDWord(_process->sp + val32));
									DO_TRACE(" mov_eax: %08x from memory[%08x + %08x]\n", _eax, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						default: 
							unknownOpcode();
							return;
					}
					break;
				
				case 0x8D:
					opcode = getPC();
					switch (opcode)
					{
						case 0x0C:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									CODE(_ecx = _process->sp);
									DO_TRACE(" lea_ecx,[esp] %08x\n", _ecx);
									break;
									
								default:
									unknownOpcode();
									return;
							}
							break;

						case 0x84:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_eax = _process->sp + val32);
									DO_TRACE(" lea_eax: %08x for memory[%08x + %02x]\n", _eax, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x85:
							val32 = getLongPC();
							CODE_V32(_eax = _ebp + val32);
							DO_TRACE(" lea_eax: %08x for memory[%08x + %02x]\n", _eax, _process->sp, val32);
							break;

						case 0x8C:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_ecx = _process->sp + val32);
									DO_TRACE(" lea_ecx: %08x for memory[%08x + %02x]\n", _ecx, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x94:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_edx = _process->sp + val32);
									DO_TRACE(" lea_edx: %08x for memory[%08x + %02x]\n", _edx, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x9C:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_ebx = _process->sp + val32);
									DO_TRACE(" lea_ebx: %08x for memory[%08x + %02x]\n", _ebx, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0x93:
					{
						CODE(val32 = _eax; _eax = _ebx; _ebx = val32);
						DO_TRACE(" xchg eax ebx: %08x %08x\n", _eax, _ebx);
					}
					break;
					
				case 0x9C: CODE(_process->push(_flags)); DO_TRACE(" push_flags\n"); break;
				
				case 0x9D: CODE(_flags = _process->pop()); DO_TRACE(" pop_flags: %d\n", _flags); break;

				case 0x99:
					CODE(_edx = (_eax & (1L << 31)) != 0 ? 0xFFFFFFFFL : 0L);
					DO_TRACE(" cdq %08x: %08x\n", _eax, _edx);
					break;
				
				case 0xA0:
					val32 = getLongPC();
					CODE_V32(set_al(_process->loadByte(val32)));
					DO_TRACE(" mov_al: %08x from memory[%08x]\n", _eax, val32);
					break;
						
				case 0xA1:
					val32 = getLongPC();
					CODE_V32(_eax = _process->loadDWord(val32));
					DO_TRACE(" mov_eax: %08x from memory[%08x]\n", _eax, val32);
					break;
						
				case 0xA2:
					val32 = getLongPC();
					CODE_V32(_process->storeByte(val32, _eax & 0xFF));
					DO_TRACE(" mov_al: %02x to memory[%08x]\n", _eax & 0xFF, val32);
					break;
						
				case 0xA3:
					val32 = getLongPC();
					CODE_V32(_process->storeDWord(val32, _eax));
					DO_TRACE(" mov_eax: %08x to memory[%08x]\n", _eax, val32);
					break;
				
				case 0xB8: val32 = getLongPC(); CODE_V32(_eax = val32); DO_TRACE(" mov_eax, %08x\n", _eax); break;
				case 0xB9: val32 = getLongPC(); CODE_V32(_ecx = val32); DO_TRACE(" mov_ecx, %08x\n", _ecx); break;
				case 0xBA: val32 = getLongPC(); CODE_V32(_edx = val32); DO_TRACE(" mov_edx, %08x\n", _edx); break;
				case 0xBB: val32 = getLongPC(); CODE_V32(_ebx = val32); DO_TRACE(" mov_ebx, %08x\n", _ebx); break;
				case 0xBD: val32 = getLongPC(); CODE_V32(_ebp = val32); DO_TRACE(" mov_ebp, %08x\n", _ebp); break;
				case 0xBE: val32 = getLongPC(); CODE_V32(_esi = val32); DO_TRACE(" mov_esi, %08x\n", _esi); break;
				case 0xBF: val32 = getLongPC(); CODE_V32(_edi = val32); DO_TRACE(" mov_edi, %08x\n", _edi); break;

				case 0xC1:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE0:
							val8 = getPC();
							CODE_V8(_eax = _eax << val8);
							DO_TRACE(" shl_eax, %d %08x\n", val8, _eax);
							break;
							
						case 0xE6:
							val8 = getPC();
							CODE_V8(_esi = _esi << val8);
							DO_TRACE(" shl_esi, %d %08x\n", val8, _esi);
							break;
							
						case 0xE7:
							val8 = getPC();
							CODE_V8(_edi = _edi << val8);
							DO_TRACE(" shl_edi, %d %08x\n", val8, _edi);
							break;
							
						case 0xE8:
							val8 = getPC();
							CODE_V8(_eax = _eax >> val8);
							DO_TRACE(" shr_eax, %d %08x\n", val8, _eax);
							break;
							
						case 0xEB:
							val8 = getPC();
							CODE_V8(_ebx = _ebx >> val8);
							DO_TRACE(" shr_eax, %d %08x\n", val8, _eax);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0xC3:
					CODE(_pc = _process->pop());
					CODE_RETURN;
					indent_depth -= 2;
					DO_TRACE(" ret to %02x (SP=%x)\n\n", _pc, _process->sp - 4);
					CALL_STACK_RETURN
#ifdef ENABLE_DO_TRACE
					if (out_trace && --nr_ret == 0)
						out_trace = false;
#endif
					break;
					
				case 0xC9:
					CODE(_process->sp = _ebp; _ebp = _process->pop());
					break;
					
				case 0xCD:
					opcode = getPC();
					DO_TRACE(" int %02x\n", opcode);
					switch (opcode)
					{
						case 0x80:
							CODE_INT;
							if (!int80())
								return;
							break;
						
						default:
#ifdef ENABLE_DO_TRACE
						    print_trace(log_file);
#endif
							fprintf(log_file, "Unknown interupt %02x\n", opcode);
							gen_program();
							return;
					}
					break;
				
				case 0xD3:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE0:
							CODE(_eax = _eax << (_ecx & 0x1F));
							DO_TRACE(" shl_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						case 0xE8:
							CODE(_eax = _eax >> (_ecx & 0x1F));
							DO_TRACE(" shr_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
													
						case 0xF0:
							CODE(_eax = _eax << (_ecx & 0x1F));
							DO_TRACE(" sal_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						case 0xF8:
							CODE(for (uint32_t i = 0; i < (_ecx & 0x1F); i++) _eax = (_eax >> 1) | (_eax & 0x80000000L));
							DO_TRACE(" sar_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						default:
#ifdef ENABLE_DO_TRACE
						    print_trace(log_file);
#endif
							fprintf(log_file, "Unknown interupt %02x\n", opcode);
							gen_program();
							return;
					}
					break;
				
				case 0xE8:
					{
						int32_t offset = (int32_t)getLongPC();
						CODE(_process->push(_pc));
						CODE_CALL(_pc + offset, 0);
						_pc += offset;
						CALL_STACK_CALL
						DO_TRACE(" call %s (SP=%x)\n\n", _process->name_for_function(_pc, -1), _process->sp);
#ifdef ENABLE_DO_TRACE
						if (do_trace_call && !do_trace) trace(" call %s (SP=%x)\n", _process->name_for_function(_pc, -1), _process->sp);
#endif
						indent_depth += 2;
					}
					break;
					
				case 0xE9:
					{
						int32_t offset = (int32_t)getLongPC();
						CODE_JUMP(true, _pc + offset);
					}
					break;
					
				case 0xEB:
					opcode = getPC();
					DO_TRACE(" jmp\n");
					CODE_JUMP(true, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;

				case 0xF7:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD0:
							CODE(_eax = ~_eax);
							DO_TRACE(" bitwnot ebp\n", opcode, _eax);
							break;
							
						case 0xD5:
							CODE(_ebp = ~_ebp);
							DO_TRACE(" bitwnot ebp\n", opcode, _ebp);
							break;
							
						case 0xE3:
							CODE(val64 = (uint64_t)_eax * (uint64_t)_ebx; _edx = (uint32_t)(val64 >> 32); _eax = (uint32_t)(val64 & 0xFFFFFFFFL)); 
							DO_TRACE(" mul_ebx\n", opcode);
							break;

						case 0xEB:
							CODE(val64 = (uint64_t)((int64_t)_eax * (int64_t)_ebx); _edx = (uint32_t)(val64 >> 32); _eax = (uint32_t)(val64 & 0xFFFFFFFFL)); 
							DO_TRACE(" imul_ebx\n", opcode);
							break;

						case 0xF3:
							CODE(val64 = ((((uint64_t)_edx) << 32) | _eax); _edx = (uint32_t)(val64 % (uint32_t)_ebx); _eax = (uint32_t)(val64 / (uint32_t)_ebx));
							DO_TRACE(" div_ebx %08x: %08x r: %08x\n", _ebx, _eax, _edx);
							break;

						case 0xFB:
							CODE(val64 = (int64_t)((((uint64_t)_edx) << 32) | _eax); _edx = (uint32_t)(val64 % (int32_t)_ebx); _eax = (uint32_t)(val64 / (int32_t)_ebx));
							DO_TRACE(" idiv_ebx %08x: %08x r: %08x\n", _ebx, _eax, _edx);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0xF4:
					fprintf(log_file, "HLT\n");
					return;
					
				case 0xFF:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD0:
							CODE(_process->push(_pc));
							CODE_CALL(_eax, "eax");
							_pc = _eax;
							CALL_STACK_CALL
							DO_TRACE(" call_eax %s (SP=%x)\n\n", _process->name_for_function(_pc, -1), _process->sp);
							indent_depth += 2;
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
						
				default:
					unknownOpcode();
					return;
			}
		}
	}
	
	bool int80()
	{
		switch (_eax)
		{
			case 0x01:
				if (!int_exit())
					return false;
				break;

			case 0x02:
				int_fork();
				break;
			
			case 0x03:
				int_read();
				break;
			
			case 0x04:
				int_write();
				break;
			
			case 0x05:
				int_open_file();
				break;
			
			case 0x06:
				int_close_file();
				break;
			
			case 0x07:
				int_wait_pid();
				break;
			
			case 0x0a:
				int_unlink();
				break;
				
			case 0x0b:
				if (!int_execve())
					return false;
				break;

			case 0x0c:
				int_chdir();
				break;
					
			case 0x0f:
				int_chmod();
				break;
					
			case 0x13:
				int_lseek();
				break;
				
			case 0x21:
				int_access();
				break;
				
			case 0x27:
				int_mkdir();
				break;
				
			case 0x29:
				int_dup();
				break;
				
			case 0x2d:
				int_sys_brk();
				break;
				
			case 0x36:
				if (!int_ioctl())
					return false;
				break;
				
			case 0x37:
				int_fcntl();
				break;
				
			case 0x4e:
				int_gettimeofday();
				break;
				
			case 0x72:
				int_wait4();
				break;
				
			case 0x7a:
				int_newuname();
				break;
				
			case 0xb7:
				int_getcwd();
				break;
				
			case 0x109:
				int_clock_gettime();
				break;
							
			default:
#ifdef ENABLE_DO_TRACE
				print_trace(log_file);
#endif
				fprintf(log_file, "Unknown system call 0x%x (%d)\n", _eax, _eax);
				gen_program();
				return false;
		}
		
		return true;
	}
	
	void gen_program()
	{
		if (do_gen)
		{
			generate_code(_process);
			output_function_addresses(_process);
			do_gen = false;
			statements = 0;
		}
	}
		
	bool int_exit()
	{
		if (do_gen)
		{
			gen_program();
			return false;
		}
		_exit_value = _ebx;
		// Exit
		if (!_exit_process())
			return false;

#ifdef ENABLE_DO_TRACE
		if (out_trace)
			return false;
#endif
		fprintf(log_file, "Continue executing process %d\n", _process->nr);

		return true;
	}	
	
	void int_open_file()
	{
		CLOSE_WRITE
		char filename[500];
		loadString(_ebx, filename, 500);
		add_cd_path(filename);
		File *file = getFile(filename);
		bool read_only = (_ecx & O_ACCMODE) == O_RDONLY;
		Usage *usage = new Usage(file, _process);
		if (read_only)
			usage->is_input(0);
		else
			usage->is_output(0);
		const char *mapped_path = file->getMappedPath(read_only);
		fprintf(log_file, " Open ProgramFile %s %o %o =>", mapped_path, _ecx, _edx);
		int fh = open(mapped_path, _ecx, _edx);
		fprintf(log_file, " %d", fh);
		if (fh <= 0)
		{
			fprintf(log_file, " (errno %d)", errno);
		}
		fprintf(log_file, "\n");
		file->fh = fh;
		_eax = fh;
		if (stat_file != 0) fprintf(stat_file, "%s %d %s\n", read_only ? "Read" : "Write", _process->nr, filename);
	}
	
	void int_close_file()
	{
		CLOSE_WRITE
		for (Usage *use = _process->uses; use != 0; use = use->next_use)
			if (use->file->fh == (int)_ebx)
			{
				fprintf(log_file, " Close file %s\n", use->file->mappedPath());
				_eax = 0;
				if (close(use->file->fh) < 0)
					_eax = errno;
				use->file->fh = -1;
				return;
			}
		_eax = -EBADF;
	}
	
	void int_wait_pid()
	{
		CLOSE_WRITE
		_eax = 0;
		if (_ecx != 0)
			_process->storeDWord(_ecx, _exit_value);
	}
	
	void int_unlink()
	{
		CLOSE_WRITE
		char filename[500];
		loadString(_ebx, filename, 500);
		add_cd_path(filename);
		const char* mapped_file = map_file(filename, /*read_only*/false);
		_eax = unlink(mapped_file);
		fprintf(log_file, "unlink '%s' mapped '%s' %d\n", filename, mapped_file, _eax); 
	}
	
	void int_fork()
	{
		CLOSE_WRITE
		if (do_gen)
		{
			generate_code(_process);
			output_function_addresses(_process);
			do_gen = false;
			statements = 0;
			fprintf(log_file, "Generated code, exit\n");
			exit(1);
		}
		_process->pc = _pc;
		_process->_ebx = _ebx;
		_process->_ecx = _ecx;
		_process->_edx = _edx;
		_process->_esi = _esi;
		_process->_edi = _edi;
		_process->_ebp = _ebp;
		fprintf(log_file, "ebp: %08x\n", _ebp);
		_process->_flags = _flags;
		_process = newProcess(_process);
		if (stat_file != 0) fprintf(stat_file, "Fork %d %d\n", _process->parent->nr, _process->nr);
		_eax = 0;
	}
	
	void int_read()
	{
		CLOSE_WRITE
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer;
			int r = read(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = -4; // EOF
				DO_TRACE(" read errno\n");
				return;
			}
			if (r == 0)
				break;
			_process->storeByte(_ecx + i, buffer);
#ifdef ENABLE_DO_TRACE
			if (do_trace)
			{
				if (buffer >= ' ' && buffer < 127)
				 	trace(" read %02x: %c to %08x\n", buffer, buffer, _ecx + i);
				else
					trace(" read %02x to %08x\n", buffer, _ecx + i);
			}
#endif
			//if (_process->nr == 8 && buffer == '&')
			//	out_trace = true;
		}
		if (i == 0)
			_eax = -4; // EOF
		DO_TRACE(" read %d bytes\n", i);
		_eax = i;
	}		

	void int_write()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer = _process->loadByte(_ecx + i);
#ifdef ENABLE_DO_TRACE
			if (do_trace)
			{
				if (buffer > ' ' && buffer < 127)
					trace(" write %02x: %c from %08x\n", buffer, buffer, _ecx + i);
				else
					trace(" write %02x from %08x\n", buffer, _ecx + i);
			}
#endif
			WRITE_CHAR(_ebx, buffer);
			int r = write(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = errno;
				DO_TRACE(" write errno %d\n", _eax);
				return;
			}
		}
		DO_TRACE(" wrote %d bytes\n", i);
		_eax = i;
	}
	
	bool int_execve()
	{
		CLOSE_WRITE
		char prog_name[500];
		loadString(_ebx, prog_name, 500);

		fprintf(log_file, " execve\n  |%s| ebx: %08x ecx: %08x edx: %08x\n", prog_name, _ebx, _ecx, _edx);
		
		int nr_sub_processes = 0;
		if (skip_process(_process->nr, nr_sub_processes, _exit_value))
		{
			fprintf(log_file, "Skip process %d\n", _process->nr);
			_process->nr_sub_processes = nr_sub_processes;
			Process::nr_of_processes += nr_sub_processes;
			return _exit_process();
		}
		
#define MAX_ARG_LEN 500
		
		int argc = 0;
		while (_process->loadDWord(_ecx + 4 * argc) != 0)
			argc++;
		char **argv = (char **)malloc(sizeof(char *) * (argc + 1));
		argv[argc] = 0;
		
		for (int j = 0; j < argc; j++)
		{
			uint32_t addr = _process->loadDWord(_ecx + 4 * j);

			char arg[MAX_ARG_LEN];
			int i = 0;
			for (; i < MAX_ARG_LEN; i++)
			{
				arg[i] = _process->loadByte(addr + i);
				if (arg[i] == 0)
					break;
			}
			fprintf(log_file, " %d %08x |%s|\n", j, addr, arg);
			argv[j] = copystr(arg);
		}
		
#define MAX_ENV_LEN 500
		
		int envc = 0;
		while (_process->loadDWord(_edx + 4 * envc) != 0)
			envc++;
		char **env = (char **)malloc(sizeof(char *) * (envc + 1));
		env[envc] = 0;
		
		for (int j = 0; j < envc; j++)
		{
			uint32_t addr = _process->loadDWord(_edx + 4 * j);

			char arg[MAX_ENV_LEN];
			int i = 0;
			for (; i < MAX_ENV_LEN; i++)
			{
				arg[i] = _process->loadByte(addr + i);
				if (arg[i] == 0)
					break;
			}
			fprintf(log_file, " env %d %08x |%s|\n", j, addr, arg);
			env[j] = copystr(arg);
		}
		
		add_cd_path(prog_name);
		File *file = getFile(prog_name);
		Usage *usage = new Usage(file, _process);
		usage->is_exec(0);
		const char *mapped_path = file->getMappedPath(/*read_only:*/true);
		
		ProgramFile program;
		if (!program.open(mapped_path))
		{
			fprintf(log_file, "Could not read '%s'\n", mapped_path);
			return false;
		}
		
		if (!loadELF(&program, _process))
		{
			fprintf(log_file, "Failed to load '%s' as ELF\n", file->mappedPath());
			return false;
		}
		
		if (stat_file != 0) fprintf(stat_file, "Exec %d %s\n", _process->nr, prog_name);
		
		_process->init(argc, argv, env);
		_pc = _process->pc;
		_eax = 0;
		_ebx = 0;
		_ecx = 0;
		_edx = 0;
		_esi = 0;
		_edi = 0;
		_ebp = 0;
		fprintf(log_file, "Start running process %d\n", _process->nr);
		if (_process->nr == 227)
		{
#ifdef ENABLE_DO_TRACE
			do_trace = true;
			//out_trace = true;
#endif
#ifdef TRACE_MEMORY
			//trace_mem = true;
#endif
		}
		if (_process->nr == gen_program_for && _process->functionNames == 0)
		{
			_process->functionNames = read_function_names();
			init_statements(_process->start_code, _process->end_code);
			start_pc = _process->pc;
		}

		return true;
	}
	
	void int_chdir()
	{
		CLOSE_WRITE
		char filename[500];
		loadString(_ebx, filename, 500);
		DO_TRACE(" chdir(\"%s\") ", filename);
		add_cd_path(filename);
		if (filename[0] == '/' && filename[1] == '\0')
		{
			strcpy(cd_path, filename);
			_eax = 0;
			return;
		}
		const char* mapped_path = map_file(filename, /*read_only*/true);
		DO_TRACE("mapped: %s ", mapped_path);
		struct stat sb;
		if (stat(mapped_path, &sb) == 0 && S_ISDIR(sb.st_mode))
		{
			DO_TRACE("new path: %s\n", filename);
			strcpy(cd_path, filename);
			_eax = 0;
		}
		else
			_eax = (uint32_t)-1;
	}
	
	void int_chmod()
	{
		char filename[500];
		loadString(_ebx, filename, 500);
		add_cd_path(filename);
		const char* mapped_file = map_file(filename, /*read_only*/false);
		DO_TRACE(" chmod(\"%s\", %x)\n", filename, _ecx);
		_eax = chmod(mapped_file, _ecx);
		if (_eax != 0)
			fprintf(log_file, "chmod('%s', %x) returned %d, errno = %d\n", filename, _ecx, _eax, errno);
	}
	
	void int_lseek()
	{
		CLOSE_WRITE
		DO_TRACE(" lseek\n");
		_eax = lseek(_ebx, _ecx, _edx);
	}
	
	void int_access()
	{
		char filename[500];
		loadString(_ebx, filename, 500);
		add_cd_path(filename);
		const char *mapped_path = map_file(filename, /*read_only*/ (_ecx & W_OK) == 0);
		_eax = access(mapped_path, _ecx);
	}
	
	void int_mkdir()
	{
		char filename[500];
		loadString(_ebx, filename, 500);
		add_cd_path(filename);
		char path[500];
		strcpy(path, "result/");
		strcat(path, filename);
		fprintf(log_file, "mkdir '%s' at '%s'\n", filename, path);
		mkdir(path, _ecx);
		_eax = 0;
	}
	
	void int_dup()
	{
		_eax = dup(_ebx);
		CLOSE_WRITE
		fprintf(stdout, "dup(%d): %d\n", _ebx, _eax);
	}
	
	void int_sys_brk()
	{
		DO_TRACE(" sys_brk %08x:", _ebx);
		if (_ebx < _process->end_code)
		{
			DO_TRACE(" init on");
		}
		else
		{
			if (_process->increase_brk(_ebx))
				DO_TRACE(" extend to");
		}
		_eax = _process->brk;
		DO_TRACE(" %08x\n", _eax);
	}
	
#define LOAD_BYTES(A,V) if (A != 0) { for (uint32_t i = 0; i < sizeof(V); i++) _process->storeByte((A) + i, ((byte*)&(V))[i]); }
#define STORE_BYTES(A,V) if (A != 0) { for (uint32_t i = 0; i < sizeof(V); i++) ((byte*)&(V))[i] = _process->loadByte((A) + i); }

	bool int_ioctl()
	{
		switch (_ecx)
		{
			case 0x5401: // TCGETS
				{
					struct termios term;
					_eax = ioctl(_ebx, _ecx, &term);
					STORE_BYTES(_edx, term);
					DO_TRACE(" sys_ioctl(TCGETS, %x, %x) : %d\n", _ebx, _ecx, _edx, _eax);
					CLOSE_WRITE
					fprintf(log_file, " sys_ioctl(%d, %x, %x) : %d\n", _ebx, _ecx, _edx, _eax);
				}
				break;
			default:
				fprintf(log_file, "Unsupported ioctl call 0x%x (%d)\n", _ebx, _ecx);
				gen_program();
				return false;
		}
		return true;
	}
	
	void int_fcntl()
	{
		_eax = fcntl(_ebx, _ecx, _edx);
		DO_TRACE(" sys_fcntl(%d, %x, %x) : %d\n", _ebx, _ecx, _edx, _eax);
		CLOSE_WRITE
		fprintf(log_file, " sys_fcntl(%d, %x, %x) : %d\n", _ebx, _ecx, _edx, _eax);
	}
	
	void int_gettimeofday()
	{
		struct timeval tval;
		LOAD_BYTES(_ebx, tval);

		struct timezone tzone;
		LOAD_BYTES(_ecx, tzone);

		_eax = gettimeofday(&tval, _ecx != 0 ? &tzone : 0);

		STORE_BYTES(_ebx, tval);
		STORE_BYTES(_ecx, tzone);
		
		CLOSE_WRITE
		fprintf(log_file, " gettimeofday\n");
	}
	
	void int_wait4()
	{
		_eax = 0;
		if (_ecx != 0)
			_process->storeDWord(_ecx, _exit_value);
		// Ignore 'options' in _edx
		if (_esi != 0)
		{
			for (uint32_t i = 0; i < sizeof(struct rusage); i++)
				_process->storeByte(_esi + i, 0);
		}
		CLOSE_WRITE
		fprintf(log_file, " wait4\n");
	}
	
	void int_newuname()
	{
		struct utsname name;
		uname(&name);
		if (_ebx != 0)
		{
			for (uint32_t i = 0; i < sizeof(struct utsname); i++)
				_process->storeByte(_ebx + i, 0);
		}
		CLOSE_WRITE
		fprintf(log_file, " newuname\n");
	}
	
	void int_getcwd()
	{
		uint32_t buf_addr = _ebx;
		uint32_t size = _ecx;
		uint32_t cd_path_len = strlen(cd_path);
		fprintf(log_file, "int_getcwd %x %d '%s' %d\n", buf_addr, size, cd_path, cd_path_len); 
		if (size < cd_path_len + 1)
			_eax = 0;
		else
		{
			_eax = _ebx;
			if (cd_path_len == 0)
			{
				_process->storeByte(buf_addr, '/');
				_process->storeByte(buf_addr + 1, '\0');
			}
			else
			{
				for (uint32_t i = 0; i <= cd_path_len; i++)
					_process->storeByte(buf_addr + i, cd_path[i]);
			}
		}
	}
	
	void int_clock_gettime()
	{
		clockid_t clockid = (clockid_t)_ebx;
		struct timespec tp;
		_eax = clock_gettime(clockid, &tp);
		fprintf(log_file, "clock_gettime %x:", _ecx);
		CLOSE_WRITE
		for (uint32_t i = 0; i < sizeof(struct timespec); i++)
			fprintf(log_file, " %02x", ((byte*)&tp)[i]);
		STORE_BYTES(_ecx, tp);
		fprintf(log_file, "\n");
	}
	
	void unknownOpcode()
	{
#ifdef ENABLE_DO_TRACE
		print_trace(log_file);
#endif
		gen_program();
		fprintf(log_file, "Unknown opcode in %s\n", _process->name);
	}


protected:
	byte getPC()
	{
#ifdef ENABLE_DO_TRACE
		byte v = _process->loadByte(_pc);
		//DO_TRACE("pc = %08x %02x\n", _pc, v);
		if (do_trace) trace_ni("%02x ", v);
		_pc++;
		return v;
#else
		return _process->loadByte(_pc++);
#endif
	}
	unsigned short getShortPC()
	{
		unsigned short val = getPC();
		val |= (unsigned short)getPC() << 8;
		return val;
	}
	uint32_t getLongPC()
	{
		uint32_t val = getShortPC();
		val |= (uint32_t)getShortPC() << 16;
		return val;
	}
	
	void loadString(uint32_t addr, char *str, int len)
	{
		for (int i = 0; i < len-1; i++, addr++)
		{
			char ch = _process->loadByte(addr);
			str[i] = ch;
			if (ch == '\0')
				return;
		}
		str[len-1] = '\0';
	}
	
	void set_al(byte val) { _eax = (_eax & 0xffffff00L) | (uint32_t)val; DO_TRACE(" eax = %08x\n", _eax); } 
	void set_bl(byte val) { _ebx = (_ebx & 0xffffff00L) | (uint32_t)val; DO_TRACE(" ebx = %08x\n", _ebx); } 
	void set_cl(byte val) { _ecx = (_ecx & 0xffffff00L) | (uint32_t)val; DO_TRACE(" ecx = %08x\n", _ecx); } 
	void set_cx(unsigned short val) { _ecx = (_ecx & 0xffff0000L) | (uint32_t)val; DO_TRACE(" ecx = %08x\n", _ecx); } 
	void set_dx(unsigned short val) { _edx = (_edx & 0xffff0000L) | (uint32_t)val; DO_TRACE(" edx = %08x\n", _edx);} 

	Process *_process;
	uint32_t _pc;
	uint32_t _eax;
	uint32_t _ebx;
	uint32_t _ecx;
	uint32_t _edx;
	uint32_t _esi;
	uint32_t _edi;
	uint32_t _ebp;
	int32_t _flags;

	uint32_t _exit_value;
	
// For debugging generated programs:
public:
	void print_string(FILE* fout, const char *name, uint32_t addr)
	{
		fprintf(fout, "String %s %08x '", name, addr);
		for (int i = 0; i < 20; i++)
		{
			byte b = _process->loadByte(addr);
			if (b == '\0')
			{
				fprintf(fout, "'");
				break;
			}
			if (b >= ' ' && b < 127)
				fprintf(fout, "%c", b);
			else
				fprintf(fout, "\\%02X", b);
			addr++;
		}
		fprintf(log_file, "\n");
	}

private:
	bool _exit_process()
	{
		completed_process(_process->nr, _process->nr_sub_processes, _exit_value);
		if (_process->parent == 0)
			return false;
		_process->finish();
		_process = _process->parent;
		_pc = _process->pc;
		_eax = 1;
		_ebx = _process->_ebx;
		_ecx = _process->_ecx;
		_edx = _process->_edx;
		_esi = _process->_esi;
		_edi = _process->_edi;
		_ebp = _process->_ebp;
		fprintf(log_file, "ebp: %08x\n", _ebp);
		_flags = _process->_flags;
		return true;
	}	
};

Process *mainProcess(int argc, char *argv[])
{
	if (argc == 1)
	{
		printf("Usage:\n  %s [-l <logfilename>] [-gen <procnr>]", argv[0]);
#ifdef ENABLE_DO_TRACE
		printf(" [-trace]");
#endif
#ifdef TRACE_MEMORY
		printf(" [-trace_mem]");
#endif
		printf(" <sourced_dir> <exec> [<args>]\n");
		return 0;
	}
			
	int i = 1;
	source_dir = 0;
	while (i < argc)
	{
		const char *arg = argv[i];
		if (strcmp(arg, "-l") == 0 && i + 1 < argc)
		{
			log_file = fopen(argv[i+1], "w");
			if (log_file == 0)
			{
				fprintf(log_file, "Cannot write to '%s'\n", argv[i+1]);
				return 0;
			}
			i += 2;
			fprintf(stderr, "Log file: '%s'\n", argv[i+1]);
		}
		else if (strcmp(arg, "-gen") == 0 && i + 1 < argc)
		{
			gen_program_for = atoi(argv[i+1]);
			i += 2;
			fprintf(stderr, "Generate program for %d\n", gen_program_for);
		}
#ifdef ENABLE_DO_TRACE
		else if (strcmp(arg, "-trace") == 0)
		{
			fprintf(stderr, "DO TRACE\n");
			do_trace = true;
			i++;
		}
		else if (strcmp(arg, "-trace_all") == 0)
		{
			fprintf(stderr, "DO TRACE\n");
			do_trace = true;
			out_trace = true;
			i++;
		}
#endif
#ifdef TRACE_MEMORY
		else if (strcmp(arg, "-trace_mem") == 0)
		{
			fprintf(stderr, "DO MEM TRACE\n");
			trace_mem = true;
			i++;
		}
#endif
		else if (source_dir == 0)
		{
			fprintf(stderr, "Source dir: '%s'\n", arg);
			source_dir = arg;
			i++;
		}
		else
			break;
	}
	if (source_dir == 0)
	{
		fprintf(log_file, "No source directory has been specified\n");
		return 0;
	}
	if (source_dir[strlen(source_dir)-1] != '/')
	{
		fprintf(log_file, "No source directory should end with '/'\n");
		return 0;
	}
	if (i >= argc)
	{
		fprintf(log_file, "Nothing to execute\n");
		return 0;
	}

	Process *main_process = newProcess(0);
	
	File *file = getFile(argv[i]);
	Usage *usage = new Usage(file, main_process);
	usage->is_exec(0);
	const char* mapped_path = file->getMappedPath(/*read_only*/true);

	ProgramFile program;
	if (!program.open(mapped_path))
	{
		fprintf(log_file, "Could not read '%s' ('%s')\n", argv[i], mapped_path);
		return 0;
	}
	
	if (!loadELF(&program, main_process))
	{
		fprintf(log_file, "Failed to load '%s' as ELF\n", argv[i]);
		return 0;
	}

	if (main_process->nr == gen_program_for && main_process->functionNames == 0)
	{
		main_process->functionNames = read_function_names();
		init_statements(main_process->start_code, main_process->end_code);
		start_pc = main_process->pc;
	}
	
	char *env[1] = { 0 };
	main_process->init(argc - i, argv + i, env);
	
	return main_process;
}	


#ifndef INCLUDED
void test_add_cd_path();

int main(int argc, char *argv[])
{
	stat_file = fopen("stat.txt", "w");
	
	init_skip_processes();
	
	Process *main_process = mainProcess(argc, argv);
	if (main_process == 0)
		return 0;

	Processor processor(main_process);
	processor.run();
	
	return 0;	
}

void test_add_cd_path()
{
	char filename[500];
	
	strcpy(filename, "test/");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);
	
	strcpy(filename, "../");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);
	
	strcpy(filename, "../");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);
	
	strcpy(filename, "test/");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);

	strcpy(filename, "a/b/c/");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);

	strcpy(filename, "../../");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);
	
	strcpy(filename, "../../");
	add_cd_path(filename);
	strcpy(cd_path, filename);
	fprintf(log_file, " '%s'\n", cd_path);
}

#endif
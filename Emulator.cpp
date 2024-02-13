/*
	http://ref.x86asm.net/geek.html
	https://www.felixcloutier.com/x86/
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

bool do_trace = false;
bool out_trace = false;
int nr_ret;
bool trace_mem = false;

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
	if (out_trace)
		fprintf(stdout, "%s", messages[message_nr]);
	messages[message_nr][MAX_MESSAGE_LEN-1] = '\0';
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
		fprintf(stdout, "%s", messages[message_nr]);
	if (++message_nr >= MAX_NR_MESSAGES)
	{
		message_nr = 0;
		message_round = true;
	}
	va_end(argp);
}

void print_trace(FILE *f)
{
	printf("---\n");
	if (message_round)
		for (int i = message_nr; i < MAX_NR_MESSAGES; i++)
			printf("%s", messages[i]);
	for (int i = 0; i < message_nr; i++)
		printf("%s", messages[i]);
	printf("---\n");
}

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
		if (do_trace) trace("  => jump to %08x\n\n", _pc); \
	}
#define CODE_CALL(X,R) cur_stat->call(X,R)
#define CODE_RETURN cur_stat->kind = 'r'
#define CODE_INT cur_stat->kind = 'i'; cur_stat->val32 = _eax

class Process;
class Usage;

class File
{
public:
	char *name;
	char *path;

	int nr;
	
	bool is_source;
	bool produced;
	Process *produced_by;
	bool exec;
	bool used_as_input;
	bool removed;
	
	int fh; // -1 => closed

	Usage *usages;

	File *next;
	
	File(const char *fn, int _nr) : path(0), nr(_nr), is_source(false), produced(false), produced_by(0), exec(false), used_as_input(0), removed(0), fh(-1), usages(0), next(0)
	{
		name = copystr(fn);
	}
};

File *files = 0;

File *getFile(const char *name)
{
	static int nr = 1;
	File **ref_file = &files;
	for (; *ref_file != 0; ref_file = &(*ref_file)->next)
		if (strcmp((*ref_file)->name, name) == 0)
			return *ref_file;
	File *file = new File(name, nr++);
	*ref_file = file;
	ref_file = &(*ref_file)->next;
	return file;
}


class Process
{
public:
	const char *name;
	int nr;
	int argc;
	char **argv;
	char **env;
	
	byte **memory[256];
	
	uint32_t pc;
	uint32_t sp;
	uint32_t start_code;
	uint32_t end_code;
	uint32_t brk;
	Process *parent;

	// Saved registers
	uint32_t _ebx;
	uint32_t _ecx;
	uint32_t _edx;
	uint32_t _esi;
	uint32_t _edi;
	uint32_t _ebp;
	int32_t _flags;
	
	Usage *uses;
	
	Process *next;
	
	Process(Process *_parent = 0) : parent(_parent), uses(0), next(0)
	{
		name = "";
		
		static int nr_of_processes = 0;
		nr = ++nr_of_processes;
		
		for (int i = 0; i < 256; i++)
			memory[i] = 0;
		sp = 0L;
		if (parent != 0)
		{
			pc = parent->pc;
			sp = parent->sp;
			end_code = parent->end_code;
			brk = parent->brk;
			for (int i = 0; i < 256; i++)
				if (parent->memory[i] != 0)
				{
					memory[i] = (byte**)malloc(0x100 * sizeof(byte *));
					for (int j = 0; j < 256; j++)
						if (parent->memory[i][j] == 0)
							memory[i][j] = 0;
						else
						{
							memory[i][j] = (byte*)malloc(0x10000 * sizeof(byte));
							for (int k = 0; k < 0x10000; k++)
								memory[i][j][k] = parent->memory[i][j][k];
						}
				}
		}
	}
	~Process()
	{
		for (int i = 0; i < 256; i++)
			if (memory[i] != 0)
			{
				for (int j = 0; j < 256; j++)
					free(memory[i][j]);
				free(memory[i]);
			}
	}

	void init(int argc, char *argv[], char *env[])
	{
		name = argv[0];
		this->argc = argc;
		this->argv = argv;
		this->env = env;
		
		uint32_t p = 4;
		
		int nr_env = 0;
		while (env[nr_env] != 0)
			nr_env++;
		
		push(0);	
		for (int i = nr_env - 1; i >= 0; i--)
		{
			push(p);
			for (int j = 0; ; j++)
			{
				storeByte(p++, env[i][j]);
				if (env[i][j] == 0)
					break;
			}
		}
		
		push(0);
		for (int i = argc - 1; i >= 0; i--)
		{
			push(p);
			for (int j = 0; ; j++)
			{
				storeByte(p++, argv[i][j]);
				if (argv[i][j] == 0)
					break;
			}
		}
		push(argc);
	}
	
	void finish();

private:	
	byte _loadByte(uint32_t address)
	{
		byte hi = (byte)((address >> 24) & 0xff);
		if (memory[hi] == 0)
			return 0;
		byte hi2 = (byte)((address >> 16) & 0xff);
		if (memory[hi][hi2] == 0)
			return 0;
		return memory[hi][hi2][(address & 0xffff)];
	}

	void _storeByte(uint32_t address, byte value)
	{
		byte hi = (byte)((address >> 24) & 0xff);
		if (memory[hi] == 0)
		{
			memory[hi] = (byte**)malloc(0x100 * sizeof(byte *));
			for (int i = 0; i < 256; i++)
				memory[hi][i] = 0;
		}
		byte hi2 = (byte)((address >> 16) & 0xff);
		if (memory[hi][hi2] == 0)
			memory[hi][hi2] = (byte*)malloc(0x10000 * sizeof(byte));
		memory[hi][hi2][(address & 0xffff)] = value;
	}
public:
	
	byte loadByte(uint32_t address)
	{
		byte result = _loadByte(address);
		if (trace_mem)
			printf("Load %02x from %08x\n", result, address);
		return result;
	}
				
	void storeByte(uint32_t address, byte value)
	{
		if (trace_mem)
			printf("Store %02x at %08x\n", value, address);
		_storeByte(address, value);
	}

	uint32_t loadDWord(uint32_t address)
	{
		uint32_t value = 0;
		for (int i = 0; i < 4; i++)
			value |= (uint32_t)_loadByte(address + i) << (i * 8);
		if (trace_mem)
			printf("Load %08x from %08x\n", value, address);
		return value;
	}
	
	void storeDWord(uint32_t address, uint32_t value)
	{
		if (trace_mem)
			printf("Store %08x at %08x\n", value, address);
		for (int i = 0; i < 4; i++)
		{
			_storeByte(address + i, value & 0xff);
			value = value >> 8;
		}
	}

	void push(uint32_t value)
	{
		sp -= 4;
		if (trace_mem) printf("push %08x to %08x\n", value, sp);
		if (do_trace) trace("push %08x to %08x\n", value, sp);
		_storeByte(sp    , (byte)value);
		_storeByte(sp + 1, (byte)(value >> 8));
		_storeByte(sp + 2, (byte)(value >> 16));
		_storeByte(sp + 3, (byte)(value >> 24));
	}
	
	uint32_t pop()
	{
		if (sp == 0L)
		{
			fprintf(stderr, "Stack underflow\n");
			exit(-1);
		}
		uint32_t value = 0;
		value |= ((uint32_t)_loadByte(sp));
		value |= ((uint32_t)_loadByte(sp + 1)) << 8;
		value |= ((uint32_t)_loadByte(sp + 2)) << 16;
		value |= ((uint32_t)_loadByte(sp + 3)) << 24;
		if (trace_mem) printf("pop %08x from %08x\n", value, sp);
		if (do_trace) trace("pop %08x from %08x\n", value, sp);
		sp += 4;
		
		return value;
	}
	
	bool increase_brk(uint32_t new_brk)
	{
		new_brk = (new_brk + 0xfff) & 0xfffff000;
		if (new_brk <= brk)
			return false;
		
		while (new_brk > brk)
			storeByte(++brk, 0);
		return true;
	}
};

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
FunctionName *functionNames = 0;

void read_function_names()
{
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
			printf("%08x %s\n", a, s);
			functionNames = new FunctionName(a, s, functionNames);  
		}
		fclose(ffn);
	}
}

char *name_for_function(uint32_t addr, int function_enter)
{
	printf("name for function %08x %d\n", addr, function_enter);
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
					
			fprintf(fout, "\tvoid %s()\n\t{\n", name_for_function(start_code + i, stat->function_enter));
			fprintf(fout, "\t\tindent += 2; if (trace_func) printf(\"%%*.*s%s\\n\", indent, indent, \"\");\n", name_for_function(start_code + i, stat->function_enter));
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
						fprintf(fout, "\t\tlabel%d: _print_label(%d);%s\n", stat->label_pos, stat->label_pos, stat->is_loop ? " // loop" : "");
						if (stat->gen_state == 'g')
							labels_to_go--;
						stat->gen_state = 'D';
					}

					if (stat->function_enter > 0 && stat->gen_state == ' ')
					{
						fprintf(fout, "\t\t\t\t\t\t\t%s();\n", name_for_function(start_code + k, stat->function_enter));
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
									fprintf(fout, "%s;\t\t\t\t\t%s();", stat->code, name_for_function(stat->func_calls->addr, statements[index]->function_enter));
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
							case 0x13: fprintf(fout, "int_lseek();"); break;
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
				fprintf(fout, "\t\tif (addr == 0x%08x) { %s(); return; }\n", start_code + i, name_for_function(start_code + i, stat->function_enter));
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
	fprintf(fout, "\tvoid _print_label(int n) { /* printf(\"%%*.*s- label%%d\\n\", indent, indent, \"\", n); */ }\n");
	fprintf(fout, "\tvoid _print_return()\n");
	fprintf(fout, "\t{\n");
	fprintf(fout, "\t\tprintf(\"%%*.*s=> %%d\", indent, indent, \"\", _eax);\n");
	fprintf(fout, "\t\tif (' ' <= _eax && _eax < 127) printf(\" '%%c'\", _eax);\n");
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
	fprintf(fout, "\tprocessor.%s();\n", name_for_function(start_code, 1));
	fprintf(fout, "	\n");
	fprintf(fout, "\treturn 0;\n");
	fprintf(fout, "}\n");
	
	fclose(fout);
}

void output_function_addresses()
{
	FILE *fout = fopen("functions_out.txt", "w");
	
	for (uint32_t i = 0; i < end_code - start_code; i++)
	{
		Statement *stat = statements[i];
		if (stat != 0 && stat->function_enter > 0)
			fprintf(fout, "%08x %s\n", start_code + i, name_for_function(start_code + i, stat->function_enter));
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
	printf("Close process\n");
	for (Usage *use = uses; use != 0; use = use->next_use)
		if (use->file->fh >= 0)
		{
			printf(" Close file %s\n", use->file->path);
			close(use->file->fh);
			use->file->fh = -1;
		}
}



char *root_dir = 0;
char *name_in_root(const char *name)
{
	static char fullname[500];
	strcpy(fullname, root_dir);
	if (name[0] == '.' && name[1] == '/')
		name += 2;
	strcat(fullname, name);
	return fullname;
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
		printf("Length %08x\n", length);
		//for (uint32_t i = 0; i < length; i++)
		//	if (do_trace) trace("%02X ", data[i]);
		//if (do_trace) trace("\n");
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
	
	uint32_t readLong(uint32_t &i)
	{
		uint32_t result = 0;
		result |= (uint32_t)data[i++];
		result |= (uint32_t)data[i++] << 8;
		result |= (uint32_t)data[i++] << 16;
		result |= (uint32_t)data[i++] << 24;
		if (do_trace) trace("readLong %08x %08x\n", i - 4, result);
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
			fprintf(stderr, "ELF signature %2d %02X %02X\n", i, signature[i], file->data[i]);
			return false;
		}
	uint32_t pc = file->readLong(i);
	process->pc = pc;
	uint32_t phoff = file->readLong(i);
	if (phoff != 0x34)
	{
		fprintf(stderr, "Program header = %08x\n", phoff);
		return false;
	}
	uint32_t section_header = file->readLong(i);
	if (section_header != 0)
	{
		fprintf(stderr, "Section header = %08x\n", section_header);
		return false;
	}
	if (file->readLong(i) != 0)
	{
		fprintf(stderr, "e_flags = %08x\n", section_header);
		return false;
	}
	unsigned short ehsize = file->readShort(i);
	if (ehsize != 0x34)
	{
		fprintf(stderr, "ehsize = %04x\n", ehsize);
		return false;
	}
	unsigned short phentsize = file->readShort(i);
	if (phentsize != 0x20)
	{
		fprintf(stderr, "phentsize = %04x\n", phentsize);
		return false;
	}
	unsigned short phnum = file->readShort(i);
	if (phnum != 1)
	{
		fprintf(stderr, "phnum = %04x\n", phnum);
		return false;
	}
	/*unsigned short shentsize =*/ file->readShort(i);
	/*unsigned short shnum =*/ file->readShort(i);
	/*unsigned short shstrndx =*/ file->readShort(i);

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
			fprintf(stderr, "ph_type = %08x\n", ph_type);
			return false;
		}
		if (ph_offset != 0)
		{
			fprintf(stderr, "ph_offset = %08x\n", ph_offset);
			return false;
		}
		if (ph_vaddr != ph_physaddr)
		{
			fprintf(stderr, "ph_vaddr %08x %08x\n", ph_vaddr, ph_physaddr);
			return false;
		}
		if (ph_filesz != ph_memsz)
		{
			fprintf(stderr, "ph_vaddr %08x %08x\n", ph_filesz, ph_memsz);
			return false;
		}
		if (ph_flags != 7)
		{
			fprintf(stderr, "ph_flags = %08x\n", ph_flags);
			return false;
		}
		if (ph_align != 1)
		{
			fprintf(stderr, "ph_align = %08x\n", ph_align);
			return false;
		}
		
		uint32_t to_mem = ph_vaddr + from_file;
		process->start_code = to_mem;
		for (uint32_t j = 0; j < ph_filesz; j++)
		{
			//printf("Load byte from %08x: %02x ", from_file, file->data[from_file]);
			process->storeByte(to_mem, file->data[from_file]);
			//printf("Stored at %08x: %02x\n", to_mem, process->loadByte(to_mem)); 
			from_file++;
			to_mem++;
		}
		process->end_code = to_mem;
		process->brk = to_mem;
		process->increase_brk(to_mem);
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
			START_INST(_pc);
			if (do_trace) trace("%08x ", _pc);
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
							if (do_trace) trace(" add_ebx,eax %08x\n", _ebx);
							break;
							
						case 0xC8:
							CODE(_eax += _ecx);
							if (do_trace) trace(" add_eax,ecx %08x\n", _eax);
							break;
							
						case 0xD8:
							CODE(_eax += _ebx);
							if (do_trace) trace(" add_eax,ebx %08x\n", _eax);
							break;
						
						case 0xE8:
							CODE(_eax += _ebp);
							if (do_trace) trace(" add_eax,ebp %08x\n", _eax);
							break;
							
						case 0xF0:
							CODE(_eax += _esi);
							if (do_trace) trace(" add_eax,esi %08x\n", _eax);
							break;
							
						case 0xF8:
							CODE(_eax += _edi);
							if (do_trace) trace(" add_eax,edi %08x\n", _eax);
							break;
							
						case 0xF9:
							CODE(_ecx += _edi);
							if (do_trace) trace(" add_ecx,edi %08x\n", _ecx);
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
							if (do_trace) trace(" add_eax,memory[%08x]: %08x\n", val32, _eax);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x04:
				    val8 = getPC();
					CODE_V8(set_al((byte)(_eax & 0xFF) + val8));
					if (do_trace) trace(" addi8_al %02x %08x\n", val8, _eax);
					break;
					
				case 0x05:
					val32 = getLongPC();
					CODE_V32(_eax += val32);
					if (do_trace) trace(" add_eax %08x: %08x\n", val32, _eax);
					break;
				
				case 0x09:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD8:
							CODE(_eax = _eax | _ebx);
							if (do_trace) trace(" or_eax,ebx: %08x\n", _eax);
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
							if (do_trace) trace(" je %d\n", _flags);
							CODE_JUMP(_flags == 0, _pc + val32)
							break;
							
						case 0x85:
							val32 = getLongPC();
							if (do_trace) trace(" jne %d\n", _flags);
							CODE_JUMP(_flags != 0, _pc + val32)
							break;
							
						case 0x86:
							val32 = getLongPC();
							if (do_trace) trace(" jbe %d\n", _flags);
							CODE_JUMP(_flags <= 0, _pc + val32)
							break;
							
						case 0x8C:
							val32 = getLongPC();
							if (do_trace) trace(" jl %d\n", _flags);
							CODE_JUMP(_flags < 0, _pc + val32)
							break;
							
						case 0x8E:
							val32 = getLongPC();
							if (do_trace) trace(" jg %d\n", _flags);
							CODE_JUMP(_flags <= 0, _pc + val32)
							break;
							
						case 0x8F:
							val32 = getLongPC();
							if (do_trace) trace(" jg %d\n", _flags);
							CODE_JUMP(_flags > 0, _pc + val32)
							break;
						
						case 0xAF:
							opcode = getPC();
							switch (opcode)
							{
								case 0xC3:
									CODE(_eax *= _ebx);
									if (do_trace) trace(" imul eax by ebx: %08x\n", _eax);
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
								case 0xC0:
									CODE(_eax = _eax & 0xFF);
									if (do_trace) trace(" movzx _eax: %08x\n", _eax);
									break;
									
								case 0xC9:
									CODE(_ecx = _ecx & 0xFF);
									if (do_trace) trace(" movzx _cl: %08x\n", _ecx);
									break;
									
								case 0xDB:
									CODE(_ebx = _ebx & 0xFF);
									if (do_trace) trace(" movzx _ebx: %08x\n", _ebx);
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
									if (do_trace) trace(" movsx_eax,BYTE_PTR_[eax]: %08x\n", _eax);
									break;
									
								case 0x1B:
									CODE(_ebx = SIGNEXT(_process->loadByte(_ebx)));
									if (do_trace) trace(" movsx_ebx,BYTE_PTR_[ebx]: %08x\n", _ebx);
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
									if (do_trace) trace(" setb_al: %08x\n", _eax);
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
									if (do_trace) trace(" setae_al: %08x\n", _eax);
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
									if (do_trace) trace(" sete_al: %08x\n", _eax);
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
									if (do_trace) trace(" setne_al: %08x\n", _eax);
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
									if (do_trace) trace(" setbe_al: %08x\n", _eax);
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
									if (do_trace) trace(" seta_al: %08x\n", _eax);
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
									if (do_trace) trace(" setl_al: %08x\n", _eax);
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
									if (do_trace) trace(" setge_al: %08x\n", _eax);
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
									if (do_trace) trace(" setle_al: %08x\n", _eax);
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
									if (do_trace) trace(" setg_al: %08x\n", _eax);
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
							if (do_trace) trace(" and_ebx:%08x to eax: %08xc\n", _ebx, _eax);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
				
				case 0x25:
					val32 = getLongPC();
					CODE_V32(_eax = _eax & val32);
					if (do_trace) trace(" and eax imm %08x %08x\n", val32, _eax);
					break;
					
				case 0x29:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC3:
							CODE(_ebx -= _eax);
							if (do_trace) trace(" sub_eax:%08x from ebx: %08xc\n", _eax, _ebx);
							break;
						
						case 0xD0:
							CODE(_eax -= _edx);
							if (do_trace) trace(" sub_edx:%08x from eax: %08xc\n", _edx, _eax);
							break;
						
						case 0xF8:
							CODE(_eax -= _edi);
							if (do_trace) trace(" sub_edi:%08x from eax: %08xc\n", _edi, _eax);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0x2C:
					val8 = getPC();
					if (do_trace) trace(" sub_al, %d\n", opcode);
					CODE_V8(set_al((byte)(_eax & 0xFF) - val8));
					break;
					 
				case 0x31:
					opcode = getPC();
					switch(opcode)
					{
						case 0xC0:
							CODE(_eax = 0L);
							if (do_trace) trace(" xor_eax,eax\n");
							break;
						
						case 0xC9:
							CODE(_ecx = 0L);
							if (do_trace) trace(" xor_ecx,ecx\n");
							break;
						
						case 0xD2:
							CODE(_edx = 0L);
							if (do_trace) trace(" xor_edx,edx\n");
							break;

						case 0xD8:
							CODE(_eax = _eax ^ _ebx);
							if (do_trace) trace(" xor_eax,ebx\n");
							break;

						case 0xDB:
							CODE(_ebx = 0L);
							if (do_trace) trace(" xor_ebx,ebx\n");
							break;

						case 0xED:
							CODE(_ebp = 0L);
							if (do_trace) trace(" xor_ebp,ebp\n");
							break;

						case 0xFF:
							CODE(_edi = 0L);
							if (do_trace) trace(" xor_edi,edi\n");
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
							if (do_trace) trace(" cmp_al_bl %08x %08x %d\n", _eax, _ebx, _flags);
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
							if (do_trace) trace(" cmp_ebx,eax\n");
							break;

						case 0xC8: //11 001 ECX 000 EAX CMP_EAX_ECX
							CODE(_flags = _eax - _ecx);
							if (do_trace) trace(" cmp_eax,ecx\n");
							break;

						case 0xCB: // 11 001 ECX 011 EBX CMP_ECX_EBX
							CODE(_flags = _ebx - _ecx);
							if (do_trace) trace(" cmp_ecx,ebx\n");
							break;

						case 0xD3: // 11 010 EDX 011 EBX CMP_EBX_EDX
							CODE(_flags = _ebx - _edx);
							if (do_trace) trace(" cmp_ebx,edx\n");
							break;

						case 0xD8: // 11 011 EBX 000 EAX CMP_EAX_EBX
							CODE(_flags = _eax - _ebx);
							if (do_trace) trace(" cmp_eax,ebx\n");
							break;

						case 0xD9: // 11 011 EBX 001 ECX CMP_EBX_ECX
							CODE(_flags = _ecx - _ebx);
							if (do_trace) trace(" cmp_ebx,ecx\n");
							break;

						case 0xFE: // 11 111 EDI 110 ESI CMP_EDI_ESI
							CODE(_flags = _esi - _edi);
							if (do_trace) trace(" cmp_edi,esi\n");
							break;

						default:
							unknownOpcode();
							return;
					}
					break;
						
				case 0x3C:
					val8 = getPC();
					CODE_V8(_flags = SIGNEXT(_eax & 0xFF) - SIGNEXT(val8));
					if (do_trace) trace(" cmp_al, %d = %d\n", val8, _flags);
					break;
					
				case 0x3D:
					val32 = getLongPC();
					CODE_V32(_flags = _eax - val32);
					if (do_trace) trace(" cmp_eax, %d = %d\n", val32, _flags);
					break;
					
				case 0x4D:
					CODE(_ebp--);
					if (do_trace) trace(" dec ebp: %08x\n", _ebp);
					break;
					
				case 0x50: CODE(_process->push(_eax)); if (do_trace) trace(" push_eax\n"); break;
				case 0x51: CODE(_process->push(_ecx)); if (do_trace) trace(" push_ecx\n"); break;
				case 0x52: CODE(_process->push(_edx)); if (do_trace) trace(" push_edx\n"); break;
				case 0x53: CODE(_process->push(_ebx)); if (do_trace) trace(" push_ebx\n"); break;
				case 0x55: CODE(_process->push(_ebp)); if (do_trace) trace(" push_ebp\n"); break;
				case 0x56: CODE(_process->push(_esi)); if (do_trace) trace(" push_esi\n"); break;
				case 0x57: CODE(_process->push(_edi)); if (do_trace) trace(" push_edi\n"); break;
					
				case 0x58: CODE(_eax = _process->pop()); if (do_trace) trace(" pop_eax: %08x\n", _eax); break;
				case 0x59: CODE(_ecx = _process->pop()); if (do_trace) trace(" pop_ecx: %08x\n", _ecx); break;
				case 0x5A: CODE(_edx = _process->pop()); if (do_trace) trace(" pop_edx: %08x\n", _ebx); break;
				case 0x5B: CODE(_ebx = _process->pop()); if (do_trace) trace(" pop_ebx: %08x\n", _ebx); break;
				case 0x5D: CODE(_ebp = _process->pop()); if (do_trace) trace(" pop_ebp: %08x\n", _ebp); break;
				case 0x5E: CODE(_esi = _process->pop()); if (do_trace) trace(" pop_esi: %08x\n", _esi); break;
				case 0x5F: CODE(_edi = _process->pop()); if (do_trace) trace(" pop_edi: %08x\n", _edi); break;
					
				case 0x6A:
					val8 = getPC();
					CODE_V8(_process->push(SIGNEXT(val8)));
					if (do_trace) trace(" push %02x\n", val8);
					break;
				
				case 0x6B:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							val8 = getPC();
							CODE_V8(_eax *= SIGNEXT(val8)); // Or? set_cx(getShortPC());
							if (do_trace) trace(" imuli8 eax %02x: %08x\n", opcode, _eax);
							break;
							
						case 0xED:
							val8 = getPC();
							CODE_V8(_ebp *= SIGNEXT(val8)); // Or? set_cx(getShortPC());
							if (do_trace) trace(" imuli8 ebp %02x: %08x\n", val8, _ebp);
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
					if (do_trace) trace(" je %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags == 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x75:
					opcode = getPC();
					if (do_trace) trace(" jne %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags != 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x76:
					opcode = getPC();
					if (do_trace) trace(" jbe8 %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags <= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7C:
					opcode = getPC();
					if (do_trace) trace(" jl %02X  flags = %d\n", opcode, _flags);
					CODE_JUMP(_flags < 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7D:
					opcode = getPC();
					if (do_trace) trace(" jge %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags >= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7E:
					opcode = getPC();
					if (do_trace) trace(" jle %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags <= 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x7F:
					opcode = getPC();
					if (do_trace) trace(" jg %02X %d\n", opcode, _flags);
					CODE_JUMP(_flags > 0, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;
				
				case 0x81:
					opcode = getPC();
					switch (opcode)
					{
						case 0xC0:
							val32 = getLongPC();
							CODE_V32(_eax += val32);
							if (do_trace) trace(" add_eax %08x: %08x\n", val32, _eax);
							break;

						case 0xC3:
							val32 = getLongPC();
							CODE_V32(_ebx += val32);
							if (do_trace) trace(" add_ebx %08x: %08x\n", val32, _ebx);
							break;

						case 0xC5:
							val32 = getLongPC();
							CODE_V32(_ebp += val32);
							if (do_trace) trace(" add_ebp %08x: %08x\n", val32, _ebp);
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
							if (do_trace) trace(" add_eax %02x %08x\n", val8, _eax);
							break;
							
						case 0xC1:
							val8 = getPC();
							CODE_V8(_ecx += SIGNEXT(val8));
							if (do_trace) trace(" add_ecx %02x %08x\n", val8, _ecx);
							break;
							
						case 0xC2:
							val8 = getPC();
							CODE_V8(_edx += SIGNEXT(val8));
							if (do_trace) trace(" add_edx %02x %08x\n", val8, _edx);
							break;
						
						case 0xC3:
							val8 = getPC();
							CODE_V8(_ebx += SIGNEXT(val8));
							if (do_trace) trace(" add_ebx %02x %08x\n", val8, _ebx);
							break;
						
						case 0xC5:
							val8 = getPC();
							CODE_V8(_ebp += SIGNEXT(val8));
							if (do_trace) trace(" add_ebp %02x: %08x\n", val8, _ebp);
							break;
						
						case 0xC6:
							val8 = getPC();
							CODE_V8(_esi += SIGNEXT(val8));
							if (do_trace) trace(" add_esi %02x: %08x\n", val8, _esi);
							break;
						
						case 0xC7:
							val8 = getPC();
							CODE_V8(_edi += SIGNEXT(val8));
							if (do_trace) trace(" add_edi %02x: %08x\n", val8, _edi);
							break;
						
						case 0xE0: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_eax = _eax & SIGNEXT(val8));
							if (do_trace) trace(" sub _eax %02x: %08x\n", val8, _eax);
							break;
								
						case 0xE8: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_eax -= SIGNEXT(val8));
							if (do_trace) trace(" sub _eax %02x: %08x\n", val8, _eax);
							break;
								
						case 0xE9: // https://www.felixcloutier.com/x86/sub
							val8 = getPC();
							CODE_V8(_ecx -= SIGNEXT(val8));
							if (do_trace) trace(" sub _ecx %02x: %08x\n", val8, _ecx);
							break;
								
						case 0xEE:
							val8 = getPC();
							CODE_V8(_esi -= SIGNEXT(val8));
							if (do_trace) trace(" sub _esi %02x: %08x\n", val8, _esi);
							break;
								
						case 0xF8: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _eax - SIGNEXT(val8));
							if (do_trace) trace(" cmp _eax %02x: %08x\n", val8, _flags);
							break;
								
						case 0xF9: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ecx - SIGNEXT(val8));
							if (do_trace) trace(" cmp _ecx %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFA: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _edx - SIGNEXT(val8));
							if (do_trace) trace(" cmp _edx %02x: %08x\n", val8, _flags);
							break;

						case 0xFB: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ebx - SIGNEXT(val8));
							if (do_trace) trace(" cmp _ebx %02x: %08x\n", val8, _flags);
							break;

						case 0xFD: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _ebp - SIGNEXT(val8));
							if (do_trace) trace(" cmp _ebp %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFE: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _esi - SIGNEXT(val8));
							if (do_trace) trace(" cmp _esi %02x: %08x\n", val8, _flags);
							break;
								
						case 0xFF: // https://www.felixcloutier.com/x86/cmp
							val8 = getPC();
							CODE_V8(_flags = _edi - SIGNEXT(val8));
							if (do_trace) trace(" cmp _edi %02x: %08x\n", val8, _flags);
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
							if (do_trace) trace(" test_eax,eax %08x %d\n", _eax, _flags);
							break;
							
						case 0xDB:
							CODE(_flags = (int32_t)_ebx);
							if (do_trace) trace(" test_ebx,ebx %08x %d\n", _ebx, _flags);
							break;
							
						case 0xED:
							CODE(_flags = (int32_t)_ebp);
							if (do_trace) trace(" test_ebp,ebp %08x %d\n", _ebp, _flags);
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
							if (do_trace) trace(" mov_[ecx:%08x],al %02x\n", _ecx, (byte)_eax);
							break;
					
						case 0x03:
							CODE(_process->storeByte(_ebx, (byte)_eax));
							if (do_trace) trace(" mov_[ebx:%08x],al %02x\n", _ebx, (byte)_eax);
							break;
					
						case 0x06:
							CODE(_process->storeByte(_esi, (byte)_eax));
							if (do_trace) trace(" mov_[esi:%08x],al %02x\n", _esi, (byte)_eax);
							break;
					
						case 0x0A:
							CODE(_process->storeByte(_edx, (byte)_ecx));
							if (do_trace) trace(" mov_[edx:%08x],cl %02x\n", _edx, (byte)_ecx);
							break;
					
						case 0x19:
							CODE(_process->storeByte(_ecx, (byte)_ebx));
							if (do_trace) trace(" mov_[ecx:%08x],bl %02x\n", _ecx, (byte)_ebx);
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
							if (do_trace) trace(" mov_[ecx:%08x],eax %08x\n", _ecx, _eax);
							break;
						
						case 0x02:
							CODE(_process->storeDWord(_edx, _eax));
							if (do_trace) trace(" mov_[edx:%08x],eax %08x\n", _edx, _eax);
							break;
						
						case 0x03:
							CODE(_process->storeDWord(_ebx, _eax));
							if (do_trace) trace(" mov_[ebx:%08x],eax %08x\n", _ebx, _eax);
							break;
						
						case 0x08:
							CODE(_process->storeDWord(_eax, _ecx));
							if (do_trace) trace(" mov_[eax:%08x],ecx %08x\n", _eax, _ecx);
							break;
						
						case 0x0B:
							CODE(_process->storeDWord(_ebx, _ecx));
							if (do_trace) trace(" mov_[ebx:%08x],ecx %08x\n", _ebx, _ecx);
							break;
						
						case 0x0D:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _ecx));
							if (do_trace) trace(" mov_ecx: %08x to memory[%08x]\n", _ecx, val32);
							break;
						
						case 0x15:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _edx));
							if (do_trace) trace(" mov_edx: %08x to memory[%08x]\n", _edx, val32);
							break;
						
						case 0x18:
							CODE(_process->storeDWord(_eax, _ebx));
							if (do_trace) trace(" mov_[eax:%08x],ebx %08x\n", _eax, _ebx);
							break;
						
						case 0x1D:
							val32 = getLongPC();
							CODE_V32(_process->storeDWord(val32, _ebx));
							if (do_trace) trace(" mov_ebx: %08x to memory[%08x]\n", _ebx, val32);
							break;
						
						case 0x2A:
							CODE(_process->storeDWord(_edx, _ebp));
							if (do_trace) trace(" mov_ebp: %08x to memory[edx:%08x]\n", _ebp, _edx);
							break;

						case 0x30:
							CODE(_process->storeDWord(_eax, _esi));
							if (do_trace) trace(" mov_esi: %08x to memory[eax:%08x]\n", _esi, _eax);
							break;
						
						case 0x38:
							CODE(_process->storeDWord(_eax, _edi));
							if (do_trace) trace(" mov_edi: %08x to memory[eax:%08x]\n", _edi, _eax);
							break;
						
						case 0x41:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ecx + val8, _eax));
							if (do_trace) trace(" mov_eax: %08x to memory[ecx:%08x + %02x]\n", _eax, _ecx, val8);
							break;
						
						case 0x42:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + val8, _eax));
							if (do_trace) trace(" mov_eax: %08x to memory[edx:%08x + %02x]\n", _eax, _edx, val8);
							break;
						
						case 0x45:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + val8, _eax));
							if (do_trace) trace(" mov_eax: %08x to memory[ebp:%08x + %02x]\n", _eax, _ebp, val8);
							break;
						
						case 0x48:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + val8, _ecx));
							if (do_trace) trace(" mov_ecx: %08x to memory[eax:%08x + %02x]\n", _ecx, _eax, val8);
							break;
						
						case 0x4A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + val8, _ecx));
							if (do_trace) trace(" mov_ecx: %08x to memory[edx:%08x + %02x]\n", _ecx, _edx, val8);
							break;
						
						case 0x50:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + val8, _edx));
							if (do_trace) trace(" mov_edx: %08x to memory[eax:%08x + %02x]\n", _edx, _eax, val8);
							break;
						
						case 0x55:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + val8, _edx));
							if (do_trace) trace(" mov_edx: %08x to memory[ebp:%08x + %02x]\n", _edx, _ebp, val8);
							break;
						
						case 0x58:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + val8, _ebx));
							if (do_trace) trace(" mov_ebx: %08x to memory[eax:%08x + %02x]\n", _ebx, _eax, val8);
							break;
						
						case 0x59:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ecx + val8, _ebx));
							if (do_trace) trace(" mov_ebx: %08x to memory[ecx:%08x + %02x]\n", _ebx, _ecx, val8);
							break;
						
						case 0x5A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + val8, _ebx));
							if (do_trace) trace(" mov_ebx: %08x to memory[edx:%08x + %02x]\n", _ebx, _edx, val8);
							break;
						
						case 0x6A:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + val8, _ebp));
							if (do_trace) trace(" mov_ebp: %08x to memory[edx:%08x + %02x]\n", _ebp, _edx, val8);
							break;
						
						case 0x6E:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_esi + val8, _ebp));
							if (do_trace) trace(" mov_ebp: %08x to memory[esi:%08x + %02x]\n", _ebp, _esi, val8);
							break;
						
						case 0x72:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_edx + val8, _esi));
							if (do_trace) trace(" mov_esi: %08x to memory[edx:%08x + %02x]\n", _esi, _edx, val8);
							break;
						
						case 0x75:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_ebp + val8, _esi));
							if (do_trace) trace(" mov_esi: %08x to memory[ebp:%08x + %02x]\n", _esi, _ebp, val8);
							break;
						
						case 0x78:
							val8 = getPC();
							CODE_V8(_process->storeDWord(_eax + val8, _edi));
							if (do_trace) trace(" mov_edi: %08x to memory[eax:%08x + %02x]\n", _edi, _eax, val8);
							break;
						
						case 0xC1: CODE(_ecx = _eax); if (do_trace) trace(" mov_ecx,eax = %08x\n", _ecx); break;
						case 0xC2: CODE(_edx = _eax); if (do_trace) trace(" mov_edx,eax = %08x\n", _edx); break;
						case 0xC3: CODE(_ebx = _eax); if (do_trace) trace(" mov_ebx,eax = %08x\n", _ebx); break;
						case 0xC5: CODE(_ebp = _eax); if (do_trace) trace(" mov_ebp,eax = %08x\n", _ebp); break;
						case 0xC6: CODE(_esi = _eax); if (do_trace) trace(" mov_esi,eax = %08x\n", _esi); break;
						case 0xC7: CODE(_edi = _eax); if (do_trace) trace(" mov_edi,eax = %08x\n", _edi); break;
						
						case 0xC8: CODE(_eax = _ecx); if (do_trace) trace(" mov_aex,ecx = %08x\n", _eax); break;
						case 0xCB: CODE(_ebx = _ecx); if (do_trace) trace(" mov_abx,ecx = %08x\n", _ebx); break;
						
						case 0xD0: CODE(_eax = _edx); if (do_trace) trace(" mov_eax,edx = %08x\n", _eax); break;
						case 0xD3: CODE(_ebx = _edx); if (do_trace) trace(" mov_ebx,edx = %08x\n", _ebx); break;
						case 0xD5: CODE(_ebp = _edx); if (do_trace) trace(" mov_ebp,edx = %08x\n", _ebp); break;
						
						case 0xD8: CODE(_eax = _ebx); if (do_trace) trace(" mov_eax,ebx = %08x\n", _eax); break;
						case 0xD9: CODE(_ecx = _ebx); if (do_trace) trace(" mov_ecx,ebx = %08x\n", _ecx); break;
						case 0xDA: CODE(_edx = _ebx); if (do_trace) trace(" mov_edx,ebx = %08x\n", _edx); break;
						case 0xDD: CODE(_ebp = _ebx); if (do_trace) trace(" mov_ebp,ebx = %08x\n", _ebp); break;
						case 0xDF: CODE(_edi = _ebx); if (do_trace) trace(" mov_edi,ebx = %08x\n", _edi); break;
						
						case 0xE1: CODE(_ecx = _process->sp); if (do_trace) trace(" mov_ecx,esp = %08x\n", _ecx); break;
						case 0xE5: CODE(_ebp = _process->sp); if (do_trace) trace(" mov_ebp,esp = %08x\n", _ebp); break;
						case 0xE7: CODE(_edi = _process->sp); if (do_trace) trace(" mov_edi,esp = %08x\n", _edi); break;
						
						case 0xE8: CODE(_eax = _ebp); if (do_trace) trace(" mov_eax,ebp = %08x\n", _eax); break;
						case 0xEA: CODE(_edx = _ebp); if (do_trace) trace(" mov_edx,ebp = %08x\n", _edx); break;
						case 0xEB: CODE(_ebx = _ebp); if (do_trace) trace(" mov_ebx,ebp = %08x\n", _ebx); break;
						
						case 0xF0: CODE(_eax = _esi); if (do_trace) trace(" mov_eax,esi = %08x\n", _eax); break;
						case 0xF3: CODE(_ebx = _esi); if (do_trace) trace(" mov_ebx,esi = %08x\n", _ebx); break;
						case 0xF7: CODE(_edi = _esi); if (do_trace) trace(" mov_edi,esi = %08x\n", _edi); break;
						
						case 0xF8: CODE(_eax = _edi); if (do_trace) trace(" mov_eax,edi = %08x\n", _eax); break;
						case 0xF9: CODE(_ecx = _edi); if (do_trace) trace(" mov_ecx,edi = %08x\n", _ecx); break;
						case 0xFA: CODE(_edx = _edi); if (do_trace) trace(" mov_edx,edi = %08x\n", _edx); break;
						case 0xFB: CODE(_flags = _ebx = _edi); if (do_trace) trace(" text ebx,ebx %d\n", _flags); break;
						case 0xFD: CODE(_ebp = _edi); if (do_trace) trace(" mov_ebp,edi = %08x\n", _ebp); break;
						case 0xFE: CODE(_esi = _edi); if (do_trace) trace(" mov_esi,edi = %08x\n", _esi); break;
						//case 0xFB: _flags = _ebx; if (do_trace) trace(" text ebx,ebx %d\n", _flags); break;
							
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
							if (do_trace) trace(" mov_al,[eax] %08x\n", _eax);
							break; 
							
						case 0x01:
							CODE(set_al(_process->loadByte(_ecx)));
							if (do_trace) trace(" mov_al,[ecx:%08x] %08x\n", _ecx, _eax);
							break; 
							
						case 0x02:
							CODE(set_al(_process->loadByte(_edx)));
							if (do_trace) trace(" mov_al,[edx:%08x] %08x\n", _edx, _eax);
							break; 
							
						case 0x03:
							CODE(set_al(_process->loadByte(_ebx)));
							if (do_trace) trace(" mov_al,[ebx:%08x] %08x\n", _ebx, _eax);
							break;
							
						case 0x04:
							opcode = getPC();
							switch (opcode)
							{
								case 0x0B:
									CODE(set_al(_process->loadByte(_ebx + _ecx)));
									if (do_trace) trace(" mov_al,[ebx:%08x + ecx:%08x] %08x\n", _ebx, _eax, _ecx);
									break;
								
								default:
									unknownOpcode();
									return;
							}
							break;
							
						case 0x08:
							CODE(set_cl(_process->loadByte(_eax)));
							if (do_trace) trace(" mov_cl,[ebx:%08x] %08x\n", _eax, _ecx);
							break; 
							
						case 0x0B:
							CODE(set_cl(_process->loadByte(_ebx)));
							if (do_trace) trace(" mov_cl,[ebx:%08x] %08x\n", _ebx, _ecx);
							break; 
							
						case 0x18:
							CODE(set_bl(_process->loadByte(_eax)));
							if (do_trace) trace(" mov_bl,[eax:%08x] %08x\n", _eax, _ebx);
							break; 
							
						case 0x19:
							CODE(set_bl(_process->loadByte(_ecx)));
							if (do_trace) trace(" mov_bl,[ecx:%08x] %08x\n", _ecx, _ebx);
							break; 
							
						case 0x1A:
							CODE(set_bl(_process->loadByte(_edx)));
							if (do_trace) trace(" mov_bl,[edx:%08x] %08x\n", _edx, _ebx);
							break; 
							
						case 0x1B:
							CODE(set_bl(_process->loadByte(_ebx)));
							if (do_trace) trace(" mov_bl,[ebx] %08x\n", _ebx);
							break; 
							
						case 0x4B:
							val8 = getPC();
							CODE_V8(set_cl(_process->loadByte(_ebx + val8)));
							if (do_trace) trace(" mov_bl,[edx:%08x + %02x] %08x\n", _ebx, val8, _ecx);
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
							if (do_trace) trace(" mov_eax,[eax] %08x\n", _eax);
							break; 
						
						case 0x01:
							CODE(_eax = _process->loadDWord(_ecx));
							if (do_trace) trace(" mov_eax,[ebx:%08x] %08x\n", _ecx, _eax);
							break; 
						
						case 0x03:
							CODE(_eax = _process->loadDWord(_ebx));
							if (do_trace) trace(" mov_eax,[ebx:%08x] %08x\n", _ebx, _eax);
							break; 
						
						case 0x09:
							CODE(_ecx = _process->loadDWord(_ecx));
							if (do_trace) trace(" mov_eax,[ecx] %08x\n", _ecx);
							break; 
						
						case 0x0B:
							CODE(_ecx = _process->loadDWord(_ebx));
							if (do_trace) trace(" mov_ecx,[ebx:%08x] %08x\n", _ebx, _ecx);
							break; 

						case 0x0D:
							val32 = getLongPC();
							CODE_V32(_ecx = _process->loadDWord(val32));
							if (do_trace) trace(" mov_ecx: %08x from memory[%08x]\n", _ecx, val32);
							break;
							
						case 0x12:
							CODE(_edx = _process->loadDWord(_edx));
							if (do_trace) trace(" mov_edx,[edx] %08x\n", _edx);
							break; 
						
						case 0x1B:
							CODE(_ebx = _process->loadDWord(_ebx));
							if (do_trace) trace(" mov_ebs,[ebx] %08x\n", _ebx);
							break;
					
						case 0x1D:
							val32 = getLongPC();
							CODE_V32(_ebx = _process->loadDWord(val32));
							if (do_trace) trace(" mov_ebx: %08x from memory[%08x]\n", _ebx, val32);
							break;
							
						case 0x36:
							CODE(_esi = _process->loadDWord(_esi));
							if (do_trace) trace(" mov_esi,[esi] %08x\n", _esi);
							break; 

						case 0x40:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_eax + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[eax + %02x]\n", _eax, val8);
							break;
							
						case 0x41:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ecx + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[ecx:%08x + %02x]\n", _eax, _ecx, val8);
							break;
							
						case 0x42:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_edx + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[edx:%08x + %02x]\n", _eax, _edx, val8);
							break;
							
						case 0x43:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ebx + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[ebx:%08x + %02x]\n", _eax, _ebx, val8);
							break;
							
						case 0x45:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_ebp + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[%08x + %02x]\n", _eax, _ebp, val8);
							break;
							
						case 0x46:
							val8 = getPC();
							CODE_V8(_eax = _process->loadDWord(_esi + val8));
							if (do_trace) trace(" mov_eax: %08x from memory[%08x + %02x]\n", _eax, _esi, val8);
							break;
							
						case 0x48:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_eax + val8));
							if (do_trace) trace(" mov_ecx: %08x from memory[%08x + %02x]\n", _ecx, _eax, val8);
							break;
							
						case 0x49:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_ecx + val8));
							if (do_trace) trace(" mov_ecx: %08x from memory[ecx + %02x]\n", _ecx, val8);
							break;
							
						case 0x4A:
							val8 = getPC();
							CODE_V8(_ecx = _process->loadDWord(_edx + val8));
							if (do_trace) trace(" mov_ecx: %08x from memory[%08x + %02x]\n", _ecx, _edx, val8);
							break;
							
						case 0x52:
							val8 = getPC();
							CODE_V8(_edx = _process->loadDWord(_edx + val8));
							if (do_trace) trace(" mov_edx: %08x from memory[edx + %02x]\n", _edx, val8);
							break;
							
						case 0x56:
							val8 = getPC();
							CODE_V8(_edx = _process->loadDWord(_esi + val8));
							if (do_trace) trace(" mov_edx: %08x from memory[%08x + %02x]\n", _edx, _esi, val8);
							break;
							
						case 0x58:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_eax + val8));
							if (do_trace) trace(" mov_ebx: %08x from memory[%08x + %02x]\n", _ebx, _eax, val8);
							break;
							
						case 0x59:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_ecx + val8));
							if (do_trace) trace(" mov_ebx: %08x from memory[%08x + %02x]\n", _ebx, _ecx, val8);
							break;
							
						case 0x5B:
							val8 = getPC();
							CODE_V8(_ebx = _process->loadDWord(_ebx + val8));
							if (do_trace) trace(" mov_ebx: %08x from memory[ebx + %02x]\n", _ebx, val8);
							break;
							
						case 0x6D:
							val8 = getPC();
							CODE_V8(_ebp = _process->loadDWord(_ebp + val8));
							if (do_trace) trace(" mov_ebp: %08x from memory[ebp + %02x]\n", _ebp, val8);
							break;
							
						case 0x7A:
							val8 = getPC();
							CODE_V8(_edi = _process->loadDWord(_edx + val8));
							if (do_trace) trace(" mov_edi: %08x from memory[%08x + %02x]\n", _edi, _edx, val8);
							break;
						
						case 0x84:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_eax = _process->loadDWord(_process->sp + val32));
									if (do_trace) trace(" mov_eax: %08x from memory[%08x + %08x]\n", _eax, _process->sp, val32);
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
									if (do_trace) trace(" lea_ecx,[esp] %08x\n", _ecx);
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
									if (do_trace) trace(" lea_eax: %08x for memory[%08x + %02x]\n", _eax, _process->sp, val32);
									break;

								default:
									unknownOpcode();
									return;
							}
							break;
						
						case 0x85:
							val32 = getLongPC();
							CODE_V32(_eax = _ebp + val32);
							if (do_trace) trace(" lea_eax: %08x for memory[%08x + %02x]\n", _eax, _process->sp, val32);
							break;

						case 0x8C:
							opcode = getPC();
							switch (opcode)
							{
								case 0x24:
									val32 = getLongPC();
									CODE_V32(_ecx = _process->sp + val32);
									if (do_trace) trace(" lea_ecx: %08x for memory[%08x + %02x]\n", _ecx, _process->sp, val32);
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
									if (do_trace) trace(" lea_edx: %08x for memory[%08x + %02x]\n", _edx, _process->sp, val32);
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
									if (do_trace) trace(" lea_ebx: %08x for memory[%08x + %02x]\n", _ebx, _process->sp, val32);
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
						if (do_trace) trace(" xchg eax ebx: %08x %08x\n", _eax, _ebx);
					}
					break;
					
				case 0x9C: CODE(_process->push(_flags)); if (do_trace) trace(" push_flags\n"); break;
				
				case 0x9D: CODE(_flags = _process->pop()); if (do_trace) trace(" pop_flags: %d\n", _flags); break;

				case 0x99:
					CODE(_edx = (_eax & (1L << 31)) != 0 ? 0xFFFFFFFFL : 0L);
					if (do_trace) trace(" cdq %08x: %08x\n", _eax, _edx);
					break;
				
				case 0xA0:
					val32 = getLongPC();
					CODE_V32(set_al(_process->loadByte(val32)));
					if (do_trace) trace(" mov_al: %08x from memory[%08x]\n", _eax, val32);
					break;
						
				case 0xA1:
					val32 = getLongPC();
					CODE_V32(_eax = _process->loadDWord(val32));
					if (do_trace) trace(" mov_eax: %08x from memory[%08x]\n", _eax, val32);
					break;
						
				case 0xA2:
					val32 = getLongPC();
					CODE_V32(_process->storeByte(val32, _eax & 0xFF));
					if (do_trace) trace(" mov_al: %02x to memory[%08x]\n", _eax & 0xFF, val32);
					break;
						
				case 0xA3:
					val32 = getLongPC();
					CODE_V32(_process->storeDWord(val32, _eax));
					if (do_trace) trace(" mov_eax: %08x to memory[%08x]\n", _eax, val32);
					break;
				
				case 0xB8: val32 = getLongPC(); CODE_V32(_eax = val32); if (do_trace) trace(" mov_eax, %08x\n", _eax); break;
				case 0xB9: val32 = getLongPC(); CODE_V32(_ecx = val32); if (do_trace) trace(" mov_ecx, %08x\n", _ecx); break;
				case 0xBA: val32 = getLongPC(); CODE_V32(_edx = val32); if (do_trace) trace(" mov_edx, %08x\n", _edx); break;
				case 0xBB: val32 = getLongPC(); CODE_V32(_ebx = val32); if (do_trace) trace(" mov_ebx, %08x\n", _ebx); break;
				case 0xBD: val32 = getLongPC(); CODE_V32(_ebp = val32); if (do_trace) trace(" mov_ebp, %08x\n", _ebp); break;
				case 0xBE: val32 = getLongPC(); CODE_V32(_esi = val32); if (do_trace) trace(" mov_esi, %08x\n", _esi); break;
				case 0xBF: val32 = getLongPC(); CODE_V32(_edi = val32); if (do_trace) trace(" mov_edi, %08x\n", _edi); break;

				case 0xC1:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE0:
							val8 = getPC();
							CODE_V8(_eax = _eax << val8);
							if (do_trace) trace(" shl_eax, %d %08x\n", val8, _eax);
							break;
							
						case 0xE6:
							val8 = getPC();
							CODE_V8(_esi = _esi << val8);
							if (do_trace) trace(" shl_esi, %d %08x\n", val8, _esi);
							break;
							
						case 0xE7:
							val8 = getPC();
							CODE_V8(_edi = _edi << val8);
							if (do_trace) trace(" shl_edi, %d %08x\n", val8, _edi);
							break;
							
						case 0xE8:
							val8 = getPC();
							CODE_V8(_eax = _eax >> val8);
							if (do_trace) trace(" shr_eax, %d %08x\n", val8, _eax);
							break;
							
						case 0xEB:
							val8 = getPC();
							CODE_V8(_ebx = _ebx >> val8);
							if (do_trace) trace(" shr_eax, %d %08x\n", val8, _eax);
							break;
							
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0xC3:
					CODE(_pc = _process->pop());
					CODE_RETURN;
					if (do_trace) trace(" ret to %02x\n\n", _pc);
					indent_depth -= 2;
					if (out_trace && --nr_ret == 0)
						out_trace = false;
					break;
					
				case 0xCD:
					opcode = getPC();
					if (do_trace) trace(" int %02x\n", opcode);
					switch (opcode)
					{
						case 0x80:
							CODE_INT;
							if (!int80())
								return;
							break;
						
						default:
						    print_trace(stdout);
							printf("Unknown interupt %02x\n", opcode);
							return;
					}
					break;
				
				case 0xD3:
					opcode = getPC();
					switch (opcode)
					{
						case 0xE0:
							CODE(_eax = _eax << (_ecx & 0x1F));
							if (do_trace) trace(" shl_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						case 0xE8:
							CODE(_eax = _eax >> (_ecx & 0x1F));
							if (do_trace) trace(" shr_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
													
						case 0xF0:
							CODE(_eax = _eax << (_ecx & 0x1F));
							if (do_trace) trace(" sal_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						case 0xF8:
							CODE(for (uint32_t i = 0; i < (_ecx & 0x1F); i++) _eax = (_eax >> 1) | (_eax & 0x80000000L));
							if (do_trace) trace(" sar_eax,cl %02x: %08x\n", _ecx & 0x1F, _eax);
							break;
							
						default:
						    print_trace(stdout);
							printf("Unknown interupt %02x\n", opcode);
							return;
					}
					break;
				
				case 0xE8:
					{
						int32_t offset = (int32_t)getLongPC();
						CODE(_process->push(_pc));
						CODE_CALL(_pc + offset, 0);
						_pc += offset;
						if (do_trace) trace(" call %s\n\n", name_for_function(_pc, -1));
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
					if (do_trace) trace(" jmp\n");
					CODE_JUMP(true, _pc + opcode - (opcode >= 0x80 ? 0x100 : 0));
					break;

				case 0xF7:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD0:
							CODE(_eax = ~_eax);
							if (do_trace) trace(" bitwnot ebp\n", opcode, _eax);
							break;
							
						case 0xD5:
							CODE(_ebp = ~_ebp);
							if (do_trace) trace(" bitwnot ebp\n", opcode, _ebp);
							break;
							
						case 0xE3:
							CODE(val64 = (uint64_t)_eax * (uint64_t)_ebx; _edx = (uint32_t)(val64 >> 32); _eax = (uint32_t)(val64 & 0xFFFFFFFFL)); 
							if (do_trace) trace(" mul_ebx\n", opcode);
							break;

						case 0xEB:
							CODE(val64 = (uint64_t)((int64_t)_eax * (int64_t)_ebx); _edx = (uint32_t)(val64 >> 32); _eax = (uint32_t)(val64 & 0xFFFFFFFFL)); 
							if (do_trace) trace(" imul_ebx\n", opcode);
							break;

						case 0xF3:
							CODE(val64 = ((((uint64_t)_edx) << 32) | _eax); _edx = (uint32_t)(val64 % (uint32_t)_ebx); _eax = (uint32_t)(val64 / (uint32_t)_ebx));
							if (do_trace) trace(" div_ebx %08x: %08x r: %08x\n", _ebx, _eax, _edx);
							break;

						case 0xFB:
							CODE(val64 = (int64_t)((((uint64_t)_edx) << 32) | _eax); _edx = (uint32_t)(val64 % (int32_t)_ebx); _eax = (uint32_t)(val64 / (int32_t)_ebx));
							if (do_trace) trace(" idiv_ebx %08x: %08x r: %08x\n", _ebx, _eax, _edx);
							break;
						
						default:
							unknownOpcode();
							return;
					}
					break;
					
				case 0xFF:
					opcode = getPC();
					switch (opcode)
					{
						case 0xD0:
							CODE(_process->push(_pc));
							CODE_CALL(_eax, "eax");
							_pc = _eax;
							if (do_trace) trace(" call_eax %08x\n\n", _pc);
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
			
			case 0x0B:
				if (!int_execve())
					return false;
				break;
					
			case 0x13:
				int_lseek();
				break;
				
			case 0x2D:
				int_sys_brk();
				break;
				
			default:
			    print_trace(stdout);
				printf("Unknown system call %02x\n", _eax);
				return false;
		}
		
		return true;
	}
	
	bool int_exit()
	{
		if (do_gen)
		{
			generate_code(_process);
			output_function_addresses();
			do_gen = false;
			statements = 0;
			return false;
		}
		// Exit
		if (_process->parent == 0)
			return false;
		_process->finish();
		_process = _process->parent;
		_pc = _process->pc;
		_ebx = _process->_ebx;
		_ecx = _process->_ecx;
		_edx = _process->_edx;
		_esi = _process->_esi;
		_edi = _process->_edi;
		_ebp = _process->_ebp;
		printf("ebp: %08x\n", _ebp);
		_flags = _process->_flags;

		if (out_trace)
			return false;
		printf("Continue executing process %d\n", _process->nr);
		_eax = 1;

		return true;
	}	
	
	void int_open_file()
	{
		char filename[500];
		uint32_t is = _ebx;
		for (int i = 0; i < 499; i++, is++)
		{
			char ch = _process->loadByte(is);
			filename[i] = ch;
			if (ch == '\0')
				break;
		}
		File *file = getFile(filename);
		bool read_only = (_ecx & O_ACCMODE) == O_RDONLY;
		Usage *usage = new Usage(file, _process);
		if (read_only)
			usage->is_input(0);
		else
			usage->is_output(0);
			
		if (file->path == 0)
			file->path = copystr(read_only ? name_in_root(filename) : file->name);
		printf(" Open ProgramFile %s %x %x =>", file->path, _ecx, _edx);
		int fh = open(file->path, _ecx, _edx);
		printf(" %d\n", fh);
		if (fh <= 0)
		{
			printf(" errno %d\n", errno);
			exit(0);
		}
		file->fh = fh;
		_eax = fh;
	}
	
	void int_close_file()
	{
		for (Usage *use = _process->uses; use != 0; use = use->next_use)
			if (use->file->fh == (int)_ebx)
			{
				printf(" Close file %s\n", use->file->path);
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
		_eax = 0;
	}
	
	void int_fork()
	{
		_process->pc = _pc;
		_process->_ebx = _ebx;
		_process->_ecx = _ecx;
		_process->_edx = _edx;
		_process->_esi = _esi;
		_process->_edi = _edi;
		_process->_ebp = _ebp;
		printf("ebp: %08x\n", _ebp);
		_process->_flags = _flags;
		_process = newProcess(_process);
		_eax = 0;
	}
	
	void int_read()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer;
			int r = read(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = -4; // EOF
				if (do_trace) trace(" read errno\n");
				return;
			}
			if (r == 0)
				break;
			_process->storeByte(_ecx + i, buffer);
			if (do_trace)
			{
				if (buffer >= ' ' && buffer < 127)
				 	trace(" read %02x: %c to %08x\n", buffer, buffer, _ecx + i);
				else
					trace(" read %02x to %08x\n", buffer, _ecx + i);
			}
			//if (_process->nr == 8 && buffer == '&')
			//	out_trace = true;
		}
		if (i == 0)
			_eax = -4; // EOF
		if (do_trace) trace(" read %d bytes\n", i);
		_eax = i;
	}		

	void int_write()
	{
		uint32_t i = 0;
		for (; i < _edx; i++)
		{
			byte buffer = _process->loadByte(_ecx + i);
			if (do_trace)
			{
				if (buffer > ' ' && buffer < 127)
					trace(" write %02x: %c from %08x\n", buffer, buffer, _ecx + i);
				else
					trace(" write %02x from %08x\n", buffer, _ecx + i);
			}
			int r = write(_ebx, &buffer, 1);
			if (r == -1)
			{
				_eax = errno;
				if (do_trace) trace(" write errno %d\n", _eax);
				return;
			}
		}
		if (do_trace) trace(" wrote %d bytes\n", i);
		_eax = i;
	}
	
	bool int_execve()
	{
		char prog_name[500];
		for (int i = 0; i < 500; i++)
		{
			prog_name[i] = _process->loadByte(_ebx + i);
			if (prog_name[i] == 0)
				break;
		}

		printf(" execve\n  |%s| ebx: %08x ecx: %08x edx: %08x\n", prog_name, _ebx, _ecx, _edx);
		
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
			printf(" %d %08x |%s|\n", j, addr, arg);
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
			printf(" env %d %08x |%s|\n", j, addr, arg);
			env[j] = copystr(arg);
		}
		
		File *file = getFile(prog_name);
		Usage *usage = new Usage(file, _process);
		usage->is_exec(0);
		if (file->path == 0)
			file->path = copystr(name_in_root(prog_name));
		
		ProgramFile program;
		if (!program.open(file->path))
		{
			fprintf(stderr, "Could not read '%s'\n", file->path);
			return false;
		}
		
		if (!loadELF(&program, _process))
		{
			fprintf(stderr, "Failed to load '%s' as ELF\n", file->path);
			return false;
		}
		
		_process->init(argc, argv, env);
		_pc = _process->pc;
		_eax = 0;
		_ebx = 0;
		_ecx = 0;
		_edx = 0;
		_esi = 0;
		_edi = 0;
		_ebp = 0;
		printf("Start running process %d\n", _process->nr);
		if (_process->nr == 20)
		{
			//do_trace = true;
			//out_trace = true;
			//trace_mem = true;
		}
		if (false && _process->nr == 20)
		{
			read_function_names();
			init_statements(_process->start_code, _process->end_code);
			start_pc = _process->pc;
		}
		
		return true;
	}
	
	void int_lseek()
	{
		if (do_trace) trace(" lseek\n");
		_eax = lseek(_ebx, _ecx, _edx);
	}
	
	void int_sys_brk()
	{
		if (do_trace) trace(" sys_brk %08x:", _ebx);
		if (_ebx < _process->end_code)
		{
			if (do_trace) trace(" init on");
		}
		else
		{
			if (_process->increase_brk(_ebx))
				if (do_trace) trace(" extend to");
		}
		_eax = _process->brk;
		if (do_trace) trace(" %08x\n", _eax);
	}
	
	void unknownOpcode()
	{
		print_trace(stdout);
		printf("Unknown opcode in %s\n", _process->name);
	}

protected:
	byte getPC()
	{
		byte v = _process->loadByte(_pc);
		//if (do_trace) trace("pc = %08x %02x\n", _pc, v);
		if (do_trace) trace_ni("%02x ", v);
		_pc++;
		return v;
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
	
	void set_al(byte val) { _eax = (_eax & 0xffffff00L) | (uint32_t)val; if (do_trace) trace(" eax = %08x\n", _eax); } 
	void set_bl(byte val) { _ebx = (_ebx & 0xffffff00L) | (uint32_t)val; if (do_trace) trace(" ebx = %08x\n", _ebx); } 
	void set_cl(byte val) { _ecx = (_ecx & 0xffffff00L) | (uint32_t)val; if (do_trace) trace(" ecx = %08x\n", _ecx); } 
	void set_cx(unsigned short val) { _ecx = (_ecx & 0xffff0000L) | (uint32_t)val; if (do_trace) trace(" ecx = %08x\n", _ecx); } 
	void set_dx(unsigned short val) { _edx = (_edx & 0xffff0000L) | (uint32_t)val; if (do_trace) trace(" edx = %08x\n", _edx);} 

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
		printf("\n");
	}

};


Process *mainProcess(int argc, char *argv[])
{
	if (argc < 3)
	{
		fprintf(stderr, "No argument\n");
		return 0;
	}

	root_dir = argv[1];
		
	Process *main_process = newProcess(0);
	
	File *file = getFile(argv[2]);
	Usage *usage = new Usage(file, main_process);
	usage->is_exec(0);
	if (file->path == 0)
		file->path = copystr(name_in_root(argv[2]));

	ProgramFile program;
	if (!program.open(file->path))
	{
		fprintf(stderr, "Could not read '%s' ('%s')\n", argv[2], file->path);
		return 0;
	}
	
	if (!loadELF(&program, main_process))
	{
		fprintf(stderr, "Failed to load '%s' as ELF\n", argv[2]);
		return 0;
	}
	
	char *env[1] = { 0 };
	main_process->init(argc - 2, argv + 2, env);
	
	return main_process;
}	


#ifndef INCLUDED
int main(int argc, char *argv[])
{
	Process *main_process = mainProcess(argc, argv);
	if (main_process == 0)
		return 0;
		
	Processor processor(main_process);
	processor.run();
	
	return 0;	
}
#endif
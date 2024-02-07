#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct Token
{
	char mode;
	char *value;
	bool used_as_value;
	bool used_as_label;
	bool used_as_func;
	int func_nr;
	Token *next;
	Token(char m, char *v, int len)
	  : next(0),
		used_as_value(false),
		used_as_label(false),
		used_as_func(false) 
	{
		mode = m;
		value = (char*)malloc(len + 1);
		strncpy(value, v, len);
		value[len] = '\0';
	}
};

Token *tokens = 0;
Token **ref_next = &tokens;

Token *newToken(char mode, char *v, int len)
{
	*ref_next = new Token(mode, v, len);
	ref_next = &(*ref_next)->next;
	return *ref_next;
}

void used_as(const char *value, char f)
{
	for (Token *token = tokens; token != 0; token = token->next)
		if (token->mode == ':' && strcmp(token->value, value) == 0)
		{
			if (f == 'v')
				token->used_as_value = true;
			else if (f == 'l')
				token->used_as_label = true;
			else if (f == 'f')
				token->used_as_func = true;
			return;
		}
}

bool match(const char *s, const char *p, char *vars)
{
	char *v = vars;
	for (; *s != '\0' && *p != '\0'; s++, p++)
		if (*p == '?')
			*v++ = *s + ('A' <= *s && *s <= 'Z' ? 'a' - 'A' : 0);
		else if (*s != *p)
			return false;
	*v = '\0';
	if (*s == '\0')
	{
		if (*p == '\0')
			return true;
		if (*p == '*')
		{
			*v++ = *s;
			*v = '\0';
			return true;
		}
	}
	return false;
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		printf("argc = %d\n", argc);
	}
	FILE *fin = fopen(argv[1], "r");
	if (fin == 0)
	{
		return 0;
	}
	
	
	char buffer[200];
	while (fgets(buffer, 199, fin))
	{
		//printf("= %s\n", buffer);
		if (strncmp(buffer, "DEFINE ", 7) == 0)
			continue;
		char *s = buffer;

		for (;;)
		{
			while (*s != '\0' && *s <= ' ')
				s++;
				
			if (*s == '\0' || *s == ';')
				break;
		
			if (*s == '"' || *s == '\'')
			{
				char term = *s;
				s++;
				char stringbuf[1000];
				int i = 0;
				for (;;)
				{
					if (*s == '\0')
					{
						fgets(buffer, 199, fin);
						s = buffer;
					}
					if (*s == term)
						break;
					stringbuf[i++] = *s++;
				}
				s++;
				stringbuf[i] = '\0';
				newToken('"', stringbuf, i);
			}
			else
			{
				char mode = ' ';
				if (*s == ':' || *s == '!' || *s == '%' || *s == '&')
					mode = *s++;
					
				int i = 0;
				while (   ('a' <= s[i] && s[i] <= 'z')
				       || ('A' <= s[i] && s[i] <= 'Z')
				       || ('0' <= s[i] && s[i] <= '9')
				       || s[i] == '_'
				       || s[i] == '-')
					i++;
				newToken(mode, s, i);
				s += i;
			}
		}
	}
	
	used_as("_start", 'f');
	for (Token *token = tokens; token != 0; token = token->next)
	{
		if (strcmp(token->value, "CALL32") == 0 && token->next != 0 && token->next->mode == '%')
		{
			token = token->next;
			used_as(token->value, 'f');
		}
		else if (token->mode == '&')
			used_as(token->value, 'v');
		else if (token->mode == '%')
			used_as(token->value, 'l');
		//printf("%c %s\n", token->mode, token->value);
	}

	int func_nr = 1;
	for (Token *token = tokens; token != 0; token = token->next)
	{
		if (token->used_as_func)
			token->func_nr = func_nr++;
	}
	
	FILE *fout = fopen("program_M1.cpp", "w");
	
	int _eax_int_80 = 0;
	
	bool in_func = false;
	for (Token *token = tokens; token != 0; token = token->next)
	{
		char vars[20];
		if (token->used_as_func)
		{
			if (in_func)
				fprintf(fout, "\t}\n");
			fprintf(fout, "\n\tvoid %s()\n\t{\n\t\tindent += 2; if (trace_func) printf(\"%%*.*s%s\\n\", indent, indent, \"\");\n", token->value, token->value);
			in_func = true;
			if (token->used_as_label)
			{
				fprintf(fout, "\t\t%s: _print_label(0);\n", token->value);
			}
		}
		else if (token->used_as_value || (!in_func && token->mode == ':'))
		{
			if (in_func)
				fprintf(fout, "\t}\n\n");
			in_func = false;
			//fprintf(fout, "\tuint32_t %s;", token->value);
			if (token->next != 0 && token->next->mode == '"')
			{
				token = token->next;
				//fprintf(fout, "// ");
				//for (char *s = token->value; *s != '\0'; s++)
				//	if (*s == '\n')
				//		fprintf(fout, "\\n");
				//	else
				//		fprintf(fout, "%c", *s);
			}
			else if (token->next != 0 && strcmp(token->value, "NULL") == 0)
			{
				token = token->next;
				//fprintf(fout, "// NULL");
			}
			//fprintf(fout, "\n");
		}
		else if (match(token->value, "ADDI8_???", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\t_%s += SIGNEXT(%s);\n", vars, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "ADD_???_into_???", vars))
		{
			fprintf(fout, "\t\t_%3.3s += _%3.3s;\n", vars + 3, vars);
		}
		else if (match(token->value, "AND_???_???", vars))
		{
			fprintf(fout, "\t\t_%3.3s = _%3.3s & _%s;\n", vars, vars, vars + 3);
		}
		else if (strcmp(token->value, "ANDI32_EAX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "CALL_EAX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "CALL32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\t_process->push(_pc);\t\t\t\t\t%s();\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR CALL32\n");
		}
		else if (match(token->value, "CMPI8_???", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\t_flags = _%s - SIGNEXT(%s);\n", vars, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "CMP_???_???", vars))
		{
			fprintf(fout, "\t\t_flags = _%3.3s - _%3.3s;\n", vars, vars + 3);
		}
		else if (match(token->value, "COPY_???_to_???", vars))
		{
			fprintf(fout, "\t\t_%3.3s = _%3.3s;\n", vars + 3, vars);
		}
		else if (strcmp(token->value, "IDIV_EBX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "IMUL_EAX_by_EBX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "IMULI8_EAX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "IMULI8_EBP") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "INT_80") == 0)
		{
			switch (_eax_int_80)
			{
				case 1: fprintf(fout, "\t\tif (!int_exit()) exit(1);\n"); break;
				case 3: fprintf(fout, "\t\tint_read();\n"); break;
				case 4: fprintf(fout, "\t\tint_write();\n"); break;
				case 5: fprintf(fout, "\t\tint_open_file();\n"); break;
				case 45: fprintf(fout, "\t\tint_sys_brk();\n"); break;
				default: fprintf(fout, "\t\tint80(); // %d\n", _eax_int_80);
			}
		}
		else if (strcmp(token->value, "JBE8") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JE32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (_flags == 0) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JG32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (_flags > 0) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JG8") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JL32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (_flags < 0) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JLE32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (_flags <= 0) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JMP32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (true) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "JNE32") == 0)
		{
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\tif (_flags != 0) goto %s;\n", token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "LEA32_???_from_esp", vars))
		{
			fprintf(fout, "\t\t_%s = _process->sp;\n", vars);
		}
		else if (match(token->value, "LOAD32_Absolute32_???", vars))
		{
			if (token->next != 0 && token->next->mode == '&')
			{
				token = token->next;
				fprintf(fout, "\t\t_%s = _process->loadDWord(%s);\n", vars, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "LOAD32_???_from_???", vars))
		{
			fprintf(fout, "\t\t_%3.3s = _process->loadDWord(_%3.3s);\n", vars, vars + 3);
		}
		else if (match(token->value, "LOAD32_???_from_???_Immediate8", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\t_%3.3s = _process->loadDWord(_%3.3s + %s);\n", vars, vars + 3, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "LOAD8_?l_from_???", vars))
		{
			fprintf(fout, "\t\tset_%cl(_process->loadByte(_%s));\n", vars[0], vars + 1);
		}
		else if (match(token->value, "LOAD8_?l_from_???_Immediate8", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\tset_%cl(_process->loadByte(_%s + %s));\n", vars[0], vars + 1, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "LOADI32_???", vars))
		{
			if (strcmp(vars, "eax") == 0)
				_eax_int_80 = -1;
			if (token->next != 0 && token->next->mode == '%')
			{
				fprintf(fout, "\t\t_%s = 0x%08x;\n", vars, atoi(token->next->value));
				if (strcmp(vars, "eax") == 0)
					_eax_int_80 = atoi(token->next->value);
				token = token->next;
			}
			else if (token->next != 0 && token->next->mode == '&')
			{
				fprintf(fout, "\t\t_%s = %s;\n", vars, token->next->value);
				token = token->next;
			}
			else
				fprintf(fout, "\t\t// ERROR %s", token->value);
		}
		else if (match(token->value, "MOVZX_?l", vars))
		{
			fprintf(fout, "\t\t_e%cx = _e%cx & 0xFF;\n", vars[0], vars[0]);
		}
		else if (strcmp(token->value, "NULL") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "POP_???", vars))
		{
			fprintf(fout, "\t\t_%s = _process->pop();\n", vars);
		}
		else if (match(token->value, "PUSH_???", vars))
		{
			fprintf(fout, "\t\t_process->push(_%s);\n", vars);
		}
		else if (strcmp(token->value, "RETURN") == 0)
		{
			fprintf(fout, "\t\t_pc = _process->pop(); if (trace_func) _print_return();\n\t\tindent -= 2; return;\n");
		}
		else if (strcmp(token->value, "SALI8_EAX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "SHRI8_???", vars))
		{
			fprintf(fout, "\t\t_%s = _%s >> 1;\n", vars, vars);
		}
		else if (match(token->value, "STORE32_Absolute32_???", vars))
		{
			if (token->next != 0 && token->next->mode == '&')
			{
				fprintf(fout, "\t\t_process->storeDWord(%s, _%s);\n", token->next->value, vars);
				token = token->next;
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (   match(token->value, "STORE32_???_into_???_Immediate8", vars)
				 || match(token->value, "STORE32_???_into_???_Immedate8", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\t_process->storeDWord(_%3.3s + %s, _%3.3s);\n", vars + 3, token->value, vars);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (match(token->value, "STORE32_???_into_???", vars))
		{
			fprintf(fout, "\t\t_process->storeDWord(_%3.3s, _%3.3s);\n", vars + 3, vars);
		}
		else if (match(token->value, "STORE8_?l_into_Address_???", vars))
		{
			fprintf(fout, "\t\t_process->storeByte(_%s, (byte)_e%cx);\n", vars + 1, vars[0]);
		}
		else if (match(token->value, "SUBI8_???", vars))
		{
			if (token->next != 0 && token->next->mode == '!')
			{
				token = token->next;
				fprintf(fout, "\t\t_%s -= SIGNEXT(%s);\n", vars, token->value);
			}
			else
				fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (strcmp(token->value, "SWAP_EAX_EBX") == 0)
		{
			fprintf(fout, "\t\tERROR %s\n", token->value);
		}
		else if (token->used_as_label)
		{
			fprintf(fout, "\t\t%s: _print_label(0);\n", token->value);
		}
		else
		{
			fprintf(fout, "\t\t// %c %s\n", token->mode, token->value);
		}
	}
	fprintf(fout, "\t}\n");
}		
			
		
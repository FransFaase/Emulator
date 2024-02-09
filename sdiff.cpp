#include <stdio.h>
#include <string.h>


typedef unsigned char byte;

char *strcopy(const char *s)
{
	char *r = new char[strlen(s) + 1];
	strcpy(r, s);
	return r;
}

struct Token
{
	char *text;
	char type;
	Token *next;
	Token(char *t, char ty) : text(t), type(ty), next(0) {}
};

struct Line
{
	Token *tokens;
	Line *matching;
	Line *next;
	Line() : tokens(0), matching(0), next(0) {}
	void print(FILE *fout)
	{
		fprintf(fout, "%c", matching || tokens == 0 ? ' ' : '$');
		for (Token *token = tokens; token != 0; token = token->next)
			fprintf(fout, " %s", token->text);
		fprintf(fout, "\n");
	}
};

typedef struct hexa_hash_tree_t hexa_hash_tree_t, *hexa_hash_tree_p;

struct hexa_hash_tree_t
{	byte state;
	union
	{	char *string;
		hexa_hash_tree_p *children;
	} data;
};

char *ident_string(const char *s)
/*  Returns a unique address representing the string.
	If the string does not occure in the store, it is added and the state
	is initialized with 0.
*/
{
	static hexa_hash_tree_p hash_tree = NULL;
	hexa_hash_tree_p *r_node = &hash_tree;
	const char *vs = s;
	int depth;
	int mode = 0;

	for (depth = 0; ; depth++)
	{   hexa_hash_tree_p node = *r_node;

		if (node == NULL)
		{   node = new hexa_hash_tree_t;
			node->state = 0;
			node->data.string = strcopy(s);
			*r_node = node;
			return node->data.string;
		}

		if (node->state != 255)
		{   char *cs = node->data.string;
			hexa_hash_tree_p *children;
			unsigned short i, v = 0;

			if (*cs == *s && strcmp(cs+1, s+1) == 0)
			{   return node->data.string;
			}

			children = new hexa_hash_tree_t*[16];
			for (i = 0; i < 16; i++)
				children[i] = NULL;

			i = strlen(cs);
			if (depth <= i)
				v = ((byte)cs[depth]) & 15;
			else if (depth <= i*2)
				v = ((byte)cs[depth-i-1]) >> 4;

			children[v] = node;

			node = new hexa_hash_tree_t;
			node->state = 255;
			node->data.children = children;
			*r_node = node;
		}
		{   unsigned short v;
			if (*vs == '\0')
			{   v = 0;
				if (mode == 0)
				{   mode = 1;
					vs = s;
				}
			}
			else if (mode == 0)
				v = ((unsigned short)*vs++) & 15;
			else
				v = ((unsigned short)*vs++) >> 4;

			r_node = &node->data.children[v];
		}
	}
}

char *characters[256];
char _characters[512];

void init_characters()
{
	for (int i = 0; i < 256; i++)
	{
		_characters[i * 2] = i;
		_characters[i * 2 + 1] = '\0';
		characters[i] = _characters + i * 2;
	}
}

Line *tokenize(FILE *f)
{
	Line *result = 0;
	Line **ref_line = &result;
	
	char buffer[501];
	while (fgets(buffer, 500, f))
	{
		*ref_line = new Line();
		Token **ref_token = &(*ref_line)->tokens;
		ref_line = &(*ref_line)->next;
		
		for (char *s = buffer; *s != '\0';)
		{
			while ('\0' < *s && *s <= ' ')
				s++;
			if (*s == '\0')
				break;
			if (*s == '/')
			{
				if (s[1] == '/')
					break;
				if (s[1] == '*')
				{
					s += 2;
					for (; *s != '\0'; s++)
						if (*s == '*' && s[1] == '/')
						{
							s += 2;
							break;
						}
					continue;
				}
			}
			if (   ('a' <= *s && *s <= 'z')
				|| ('A' <= *s && *s <= 'Z')
				|| *s == '_')
			{
				char ident[100];
				char *i = ident;
				while (   ('a' <= *s && *s <= 'z')
					   || ('A' <= *s && *s <= 'Z')
				       || ('0' <= *s && *s <= '9')
				       || *s == '_')
					*i++ = *s++;
				*i = '\0';
				*ref_token = new Token(ident_string(ident), 'i');
				ref_token = &(*ref_token)->next;
			}
			else if (  ('0' <= *s && *s <= '9')
					 || (*s == '-' && ('0' <= s[1] && s[1] <= '9')))
			{
				char number[100];
				char *n = number;
				if (*s == '0' && s[1] == 'x')
				{
					*n++ = *s++;
					*n++ = *s++;
					while (   ('a' <= *s && *s <= 'f')
						   || ('A' <= *s && *s <= 'F')
						   || ('0' <= *s && *s <= '9'))
						*n++ = *s++;
				}
				else
				{
					if (*s == '-')
						*n++ = *s++;
					while ('0' <= *s && *s <= '9')
						*n++ = *s++;
				}
				*n = '\0';
				*ref_token = new Token(ident_string(number), 'n');
				ref_token = &(*ref_token)->next;
			}
			else
			{
				*ref_token = new Token(characters[*s], 's');
				ref_token = &(*ref_token)->next;
				s++;
			}
		}
	}
	return result;
}

struct Match
{
	const char *from;
	const char *to;
	Match *next;
	Match(const char *f, const char *t) : from(f), to(t), next(0) {}
};

Match *func_matches = 0;
Match *label_matches = 0;
Match *value_matches = 0;

bool addMatch(Match **ref_match, const char *from, const char *to)
{
	bool has_match = false;
	for (; *ref_match != 0; ref_match = &(*ref_match)->next)
		if ((*ref_match)->from == from)
		{
			if ((*ref_match)->to != to)
				return false;
			has_match = true;
		}
		else if ((*ref_match)->to == to)
			return false;
	if (has_match)
		return true;
	*ref_match = new Match(from, to);
	return true;
}

const char *getMatch(Match *matches, const char *from)
{
	for (Match *match = matches; match != 0; match = match->next)
		if (match->from == from)
			return match->to;
	return from;
}

/*bool doMatch(Match *matches, const char *from, const char *to)
{
	for (Match *match = matches; match != 0; match = match->next)
		if (match->from == from)
			return match->to == to;
		else if (match->to == to)
			return false;
	return false;
}
*/

struct Function
{
	const char *name;
	Function *next;
	Function(const char *n) : name(n) {}
};

Function *functions = 0;

void addFunction(const char *name)
{
	Function **ref_func = &functions;
	for (; *ref_func != 0; ref_func = &(*ref_func)->next)
		if ((*ref_func)->name == name)
			return;
	*ref_func = new Function(name);
}	

bool end_of_function(Line *line)
{
	return line == 0 || (line->tokens != 0 && line->tokens->text == characters['}']);
}

int main(int argc, char *argv[])
{
	init_characters();
	
	FILE *f1 = fopen("program.cpp", "r");
	FILE *f2 = fopen("program_M1.cpp", "r");
	if (f1 == 0 || f2 == 0)
	{
		printf("ERROR opening\n");
		return 0;
	}
	
	Line *lines1 = tokenize(f1);
	Line *lines2 = tokenize(f2);
	
	const char *s_void = ident_string("void");
	const char *s_close = characters['}'];
	const char *s_goto = ident_string("goto");
	const char *s_semicolon = characters[';'];
	const char *s_colon = characters[':'];
	const char *s_open_bracket = characters['('];
	const char *s_open = characters['{'];
	const char *s_print_label = ident_string("_print_label");
	const char *s_if = ident_string("if");
	const char *s_star = characters['*'];
	const char *s_backslash = characters['\\'];
	const char *s_true = ident_string("true");
	
	addFunction(ident_string("_start"));
	
	for (Function *function = functions; function != 0; function = function->next)
	{
		Line *line2;
		for (line2 = lines2; line2 != 0; line2 = line2->next)
			if (   line2->tokens != 0 && line2->tokens->text == s_void
			    && line2->tokens->next != 0 && line2->tokens->next->text == function->name)
			    break;
		
		const char *other_name = getMatch(func_matches, function->name);
		Line *line1;
		for (line1 = lines1; line1 != 0; line1 = line1->next)
			if (   line1->tokens != 0 && line1->tokens->text == s_void
			    && line1->tokens->next != 0 && line1->tokens->next->text == other_name)
			    break;
		
		printf("Find function %s with %s\n", function->name, other_name);
		if (line1 == 0 || line2 == 0)
		{
			if (line1 == 0)
				printf("Did not find function %s in file 1\n", other_name);
			if (line2 == 0)
				printf("Did not find function %s in file 2\n", function->name);
		}
		else
		{
			printf("Found\n");
			for (;;)
			{
				while (line2 != 0 && line2->tokens == 0)
					line2 = line2->next;
					
				//printf("Compare lines ");
				bool eof1 = end_of_function(line1);
				bool eof2 = end_of_function(line2);
				if (eof1 && eof2)
				{
					line1->matching = line2;
					line2->matching = line1;
					break;
				}
				
				if (eof1)
				{
					printf("End line1: ");
					if (line1 != 0) line1->print(stdout);
					printf("\n");
					break;
				}
				if (eof2)
				{
					printf("End line2: ");
					if (line2 != 0) line2->print(stdout);
					printf("\n");
					break;
				}
				
				Token *token1 = line1->tokens;
				Token *token2 = line2->tokens;
				for (; token1 != 0 && token2 != 0; token1 = token1->next, token2 = token2->next)
				{
					if (token1->text == s_open && token2->text == s_goto)
					{
						token1 = 0;
						token2 = 0;
						break;
					}
					if (token2->type == 'i')
					{
						if (token1->type != 'i' && token1->type != 'n')
							break;
					}
					else if (token2->type == 'n')
					{
						if (token1->type != 'n')
							break;
					}
					else if (token2->text != token1->text)
						break;
				}
				char *function_call = 0;
				if (token1 == 0 && token2 == 0)
				{
					token1 = line1->tokens;
					token2 = line2->tokens;
					char *prev_text = 0;
					for (; token1 != 0 && token2 != 0; token1 = token1->next, token2 = token2->next)
					{
						//printf("t");
						if (   token1->text == s_print_label
						    || (token1->text == s_open && token2->text == s_goto))
						{
							token1 = 0;
							token2 = 0;
							break;
						}
						if (token1->text != token2->text)
						{
							if (token1->type == 'n' && token2->type == 'n')
							{
								if (   (strcmp(token1->text, "252") != 0 || strcmp(token2->text, "-4") != 0)
								    && (strcmp(token1->text, "255") != 0 || strcmp(token2->text, "-1") != 0))
									break;
							}
							else
							{
								if (token2->type != 'i')
									break;
								if (token1->type == 'n')
								{
									if (!addMatch(&value_matches, token2->text, token1->text))
										break;
								}
								else if (token1->type != 'i')
									break;
								else if (   prev_text == s_star 
										 && token1->next != 0 && token1->next->text == s_backslash
										 && token2->next != 0 && token2->next->text == s_backslash)
								{
								}
								else if (   (prev_text == s_semicolon || prev_text == s_void)
										 && token1->next != 0 && token1->next->text == s_open_bracket
										 && addMatch(&func_matches, token2->text, token1->text))
								{
								}
								else if	(   strncmp(token1->text, "label", 5) == 0
										 && (prev_text == s_goto || (token1->next != 0 || token1->next->text == s_colon))
										 && addMatch(&label_matches, token2->text, token1->text))
								{
								}
								else
									break;
							}
						}
						if (prev_text == s_semicolon && token2->text != s_if && token2->next != 0 && token2->next->text == s_open_bracket)
							function_call = token2->text;
						
						prev_text = token1->text;
					}
				}
				//printf("\n");
				
				if (token1 != 0 || token2 != 0)
				{
					/*
					printf("Not equal ");
					if (token1 == 0)
						printf(" --");
					else
						printf(" %s", token1->text);
					if (token2 == 0)
						printf(" --");
					else
						printf(" %s", token2->text);
					printf("\n");
					line1->print(stdout);
					line2->print(stdout);
					/**/
					if (   line1->tokens != 0 && line1->tokens->type == 'i'
						&& line1->tokens->next != 0 && line1->tokens->next->text == s_colon)
					{
						bool found = false;
						for (Line *pos_line2 = line2; !found && !end_of_function(pos_line2); pos_line2 = pos_line2->next)
							if (   pos_line2->tokens != 0 && pos_line2->tokens->type == 'i'
								&& pos_line2->tokens->next != 0 && pos_line2->tokens->next->text == s_colon
								&& getMatch(label_matches, pos_line2->tokens->text) == line1->tokens->text
								)
							{
								found = true;
								line2 = pos_line2;
							}
						if (found)
						{
							continue;
						}
					}
					if (line2->tokens != 0 && line2->tokens->type == 'i' && line2->tokens->next != 0 && line2->tokens->next->text == s_colon)
					{
						// Skip label
						line2 = line2->next;
						continue;
					}
					printf("Diff\n");
					line1->print(stdout);
					line2->print(stdout);
					printf("\n");
					break;
				}
				//printf("Next\n");
				if (function_call)
					addFunction(function_call);
				line1->matching = line2;
				line2->matching = line1;
				line1 = line1->next;
				line2 = line2->next;
			}
		}
	}
	
	// Print matching:
	
	for (Match *match = func_matches; match != 0; match = match->next)
		printf("Func Match %s %s\n", match->from, match->to);
	for (Match *match = label_matches; match != 0; match = match->next)
		printf("Label Match %s %s\n", match->from, match->to);
	for (Match *match = value_matches; match != 0; match = match->next)
		printf("Value Match %s %s\n", match->from, match->to);
	printf("\n\n");
	
	for (Line *line = lines1; line != 0; line = line->next)
		line->print(stdout);
	printf("\n\n");

	for (Line *line = lines2; line != 0; line = line->next)
		line->print(stdout);
}
				

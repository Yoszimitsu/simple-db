#include "SQL_parser.h"
#include "table.h"
#include <stdbool.h>
#include <string.h>

bool parse_select(char *sql_str, Select_Query *query) {}
bool parse_insert(char *sql_str, Insert_Query *query) {}
bool parse_delete(char *sql_str, Delete_Query *query) {}
bool parse_update(char *sql_str, Update_Query *query) {}

bool parse_SQL(char *sql_str, SQL_Query *query)
{
    query->type = INSERT;
    T_Record* record = &query->query.insert_q.record;

    record->id = 1;
    record->age = 22;
    record->height = 182.3;
    strcpy(&record->name, "Johny Bowen");
}

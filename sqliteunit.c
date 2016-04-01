#include <stdio.h>   
#include <string.h> 
   
#define SQLLENGTH 256 // sql���ĳ���

/* ����sql��� */
/**---���� */
int sqlcreate(char *sql, char *table, char *key, char *kind){
	memset(sql, '\0', sizeof(sql)); // ���sql���
	strcat(sql,"create table ");
	strcat(sql,table);
	strcat(sql,"(");
	strcat(sql,key);
	strcat(sql," ");
	strcat(sql,kind);
	strcat(sql,")");
	//printf("the sql of create is:%s\n",sql);
	return 0;
}
/**---�� */
int sqlinsert(char *sql, char *table, char *key, char *value){
	memset(sql, '\0', sizeof(sql)); // ���sql���
	strcat(sql,"insert into ");
	strcat(sql,table);
	strcat(sql,"(");
	strcat(sql,key);
	strcat(sql,")");
	strcat(sql," values (");
	strcat(sql,"'");
	strcat(sql,value);
	strcat(sql,"'");
	strcat(sql,")");
	printf("the sql of insert is:%s\n",sql);
	return 0;
}

/**---ɾ */
int sqldelete(char *sql, char *table, char *key, char *value){
	memset(sql, '\0', sizeof(sql)); // ���sql���
	strcat(sql,"delete from ");
	strcat(sql,table);
	strcat(sql," where ");
	strcat(sql,key);
	strcat(sql," = ");
	strcat(sql,"'");
	strcat(sql,value);
	strcat(sql,"' ");
	//printf("the sql of delete is:%s\n",sql);
	return 0;
}

/**---�� */
int sqlselect(char *sql, char *table, char *key, char *value){
	memset(sql, '\0', sizeof(sql)); // ���sql���
	strcat(sql," select * from ");
	strcat(sql,table);
	strcat(sql," where ");
	strcat(sql,key);
	strcat(sql," = '");
	strcat(sql,value);
	strcat(sql,"'");
	//printf("the sql of select is:%s\n",sql);
	return 0;
}

/**---�ص����� */
int select_callback(void *data, int n_columns, char **column_values, char **column_names){
	int i;
	for(i = 0; i < n_columns; ++i)
		printf("column_name:%s\tcolumn_value:%s\n", column_names[i], column_values[i]);
	return 0;
}















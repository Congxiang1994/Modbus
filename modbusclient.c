#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<termios.h>
#include<errno.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<unistd.h>
#include<netinet/in.h>
#include<unistd.h>
#include<fcntl.h>
#include<sys/signal.h>
#include<pthread.h>
#include<arpa/inet.h>
#include <sqlite3.h> 

#define DEST_PORT 8000
#define DEST_IP_ADDRESS "172.29.143.144"
//#define DEST_IP_ADDRESS "127.0.0.1"

#define BAUDRATE B9600
#define COM2 "/dev/ttyS1" 
//#define COM2 "/dev/ttySAC1" 
#define TRUE 1
#define FALSE 0

#define SLEEPTIME 3 // sleep();���ʱ�䣬��λ����
#define MAXLINE 128 // ��ַ���
#define SQLLENGTH 256 // sql���ĳ���
#define MODBUSORDERLENGTH 8 // modbus����ĳ���

/* -ȫ�ֱ��������� */
volatile int com_fd; // ���ڵ��ļ�������
volatile int sock_fd; // �׽��ֵ��ļ�������

sqlite3 *db; // ���ݿ��ļ�ָ��
int res_db; // �����ж�sqlite�����Ƿ�ִ����ȷ
char sql[SQLLENGTH] = "\0" ;
char *errMsg; // ����ִ��sqlite3ִ�еĴ�����Ϣ
unsigned char ** dbResult; //�� char ** ���ͣ�����*��
int nRow, nColumn;


/* -��ʶ�������� */ 
int isOpenCom; // ��ʶ�Ƿ�򿪴���
int isConnectServer; // ��ʶ�Ƿ�����server����

/* -�߳�ͬ�������ݿ��д */
pthread_mutex_t mutex_db; // ���ݿ⻥���ź�����һ��ֻ��һ���߳��ܹ��������ݿ�

/* -���������� */
void *thread_recv(void *arg); // �����ն���server����������֮�����Ϣͨ��
void *thread_connectServer(void *arg); // ����ѯ��server�����Ƿ�����
void *thread_modbusdata(void *arg); // ����modbus�ն�����λ�豸֮���ͨ�ţ��Լ�����������

int sqlcreate(char *sql, char *table, char *key, char *kind);
int sqlinsert(char *sql, char *table, char *key, char *value);
int sqldelete(char *sql, char *table, char *key, char *value);
int sqlselect(char *sql, char *table, char *key, char *value);
int select_callback(void *data, int n_columns, char **column_values, char **column_names);


int main(){
	
	/* ��ʼ�����ݿ⻥���ź��� */
	int init = pthread_mutex_init(&mutex_db, NULL);
	if(init != 0)
	{
	   printf("main_Function:mutex_db init failed \n");
	   return 0;
	} 

	/* �������ݿ� */
	printf("main_Function:Begin to create a database...\n");
	res_db = sqlite3_open("modbus",&db);
	if(res_db != SQLITE_OK){
		fprintf(stderr, "�����ݿ�ʧ��: %s\n", sqlite3_errmsg(db));
		return 0;
	}
	
	/* �����ݿ��modbusorder */
	printf("main_Function:Begin to create table:modbusorder...\n");
	sqlcreate(sql,"modbusorder","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusorder error:%s\n", errMsg);
	}
	
	/* �����ݿ��modbusdata */
	printf("main_Function:Begin to create table:modbusdata...\n");
	sqlcreate(sql,"modbusdata","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusdata error:%s\n", errMsg);
	}
	printf("main_Function:create database and table successfully.\n\n");

	/**----------------------------------------------------*/
	
	/* �ȱ�ʶδ�����Ϸ����� */
	isConnectServer=FALSE; 
	
	/* ����һ���̣߳���������server�����������͹�������Ϣ */
	printf("main_Function: begin to create a thread [in order to connect to server]......\n");
	pthread_t thread_connectToServer;
	int res_connectToServer;
	res_connectToServer=pthread_create(&thread_connectToServer,NULL,thread_connectServer,NULL);
	if(res_connectToServer != 0){
		perror("main_Function: thread_connectToServer creat failed\n\n");
		exit(1);
	}else{
		printf("main_Function: thread_connectToServer create sucessfully\n\n");
	}

	/*  �����ѭ���������Ǳ������̲߳��رգ��������߳̾���һֱ���� */
	while(1){
		sleep(10000);
	}

}

/*---����serverû������ʱ�����

1.����һ��С�̣߳���ͣ��ѯ��server�����Ƿ�����

2.����⵽server�������ߺ�ֹͣѯ��

3.������server�������ߵ����ʱ���������С�̣߳����¿�ʼѯ��server�����Ƿ����� 

4.-�ѵ㣺c���Բ������׳��쳣����μ�⵽server�������ߵ��쳣��

*/
void *thread_connectServer(void *arg){
	
	/* ����server���������� */
	struct sockaddr_in addr_serv;
	sock_fd=socket(AF_INET,SOCK_STREAM,0);

	if(sock_fd<0){
		perror("connectServer_Thread: create socket failed\n");
		exit(1);
	}
	
	memset(&addr_serv,0,sizeof(addr_serv));
	addr_serv.sin_family=AF_INET;
	addr_serv.sin_port=htons(DEST_PORT);
	addr_serv.sin_addr.s_addr=inet_addr(DEST_IP_ADDRESS);
	
	/* ����ѭ�����ȴ�server�������� */
	while(1){
		printf("\n"); // ��һ�����У�����һ��ѭ�����̼������
		if(connect(sock_fd,(struct sockaddr *)&addr_serv,sizeof(struct sockaddr))<0){
			//perror("connectServer_Thread: connect server failed\n");
			//printf("connectServer_Thread: connect (%d) , continue to connect to server...\n",errno);			
			//exit(1);
			printf("connectServer_Thread: connect server failed , continue to connect to server...\n");
		}else{
			printf("connectServer_Thread: connect server successfully\n\n");
			isConnectServer=TRUE; // ������server��ʶ����Ϊtrue
			break;
		}
		sleep(SLEEPTIME);
	}
	
	/* ����һ���̣߳���������server�����������͹�������Ϣ */
	printf("connectServer_Thread: begin to create a thread [in order to deal with the msg from server]......\n");
	pthread_t thread_recvMsg;
	int res;
	res=pthread_create(&thread_recvMsg,NULL,thread_recv,NULL);
	if(res != 0){
		perror("connectServer_Thread: thread_recvMsg creat failed\n\n");
		exit(1);
	}else{
		printf("connectServer_Thread: thread_recvMsg create sucessfully\n\n");
	}
	
	sleep(SLEEPTIME); // ��ͣһ�£��ȴ������߳������ɹ�
	
	/* �����������͸�server�������Լ������ */
	int send_num; // ��¼���͵��ֽ���
	unsigned char send_buff[64]; // ���ͻ�����
	int recv_num; // ��¼���յ��ֽ���
	unsigned char recv_buff[64]; // ���ջ�����
	
	printf("connectServer_Thread: begin to send type [identity myself: 0x01]......\n");
	send_buff[0]=0x01; // ���������01
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	
	/* ��ʼ׼���򿪴��� */
	printf("connectServer_Thread: begin to open COM......\n");
	//���ô������� 
	struct termios oldtio,newtio;
	com_fd = open(COM2,O_RDWR);
	int isopen = TRUE;
	if(com_fd < 0){
		printf("connectServer_Thread: open COM2 failed\n");
		isopen = FALSE;
		//exit(1);
	}else{
		printf("connectServer_Thread: open COM2 successfully\n");
		tcgetattr(com_fd,&oldtio);
		tcflush(com_fd,TCIOFLUSH);
		
		cfsetispeed(&newtio,BAUDRATE);
		cfsetospeed(&newtio,BAUDRATE);
		
		newtio.c_cflag &= ~CSIZE;
		newtio.c_cflag |= CS8;
		newtio.c_cflag |= CREAD;
		newtio.c_cflag &=~PARENB;
		newtio.c_iflag &=~INPCK;
		newtio.c_cflag &=~CSTOPB;
		newtio.c_cc[VMIN]=0;
		newtio.c_cc[VTIME]=150;
		
		// ��ʼ���ô�������
		tcflush(com_fd,TCIOFLUSH);
		if(tcsetattr(com_fd,TCSANOW,&newtio) != 0){
			printf("connectServer_Thread: set COM2 attribute failed\n");
			isopen = FALSE;
			//exit(1);
		}else{
			printf("connectServer_Thread: set COM2 attribute successfully\n\n");
		}	
	}

	//���ݱ�ʶ����������ʲô�ַ���server
	if(isopen == TRUE){
		send_buff[0]=0x04; // �򿪴��ڼ����ò����ɹ�
		isOpenCom=TRUE;
		printf("connectServer_Thread: open COM state successfully\n\n");
	}else{
		send_buff[0]=0x05; // �򿪴��ڼ����ò���ʧ��
		isOpenCom=FALSE;
		printf("connectServer_Thread: open COM state failed\n\n");
	}
	
	sleep(SLEEPTIME);
	
	/* ����isopen ��ֵ�ж��Ƿ�򿪳ɹ���������������server */
	printf("connectServer_Thread: begin send type [open COM state]......\n");
	
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	
	
	/* ����һ���̣߳�����modbus�ն�����λ�豸֮���ͨ�ţ��Լ����������� */
	printf("connectServer_Thread: begin to create a thread [in order to deal with the data from device]......\n");
	pthread_t thread_modbusdataMsg;
	res=pthread_create(&thread_modbusdataMsg,NULL,thread_modbusdata,NULL);
	if(res != 0){
		perror("connectServer_Thread: thread_modbusdata creat failed\n\n");
		exit(1);
	}else{
		printf("connectServer_Thread: thread_modbusdata create sucessfully\n\n");
	}

}


/*---�����ն���server����������֮�����Ϣͨ��

1.��Ϊ��߽�������Ҳ�ǲ��ö��̣߳����ܻ���ֶ�ν��յ����

2.���յ���Ϣ�󣬲�һ����Ҫ�ٴν���Ϣ���أ���ֹ���ߴ�����������

3.����һ��ʼ�ͽ������������ݡ����̣߳��������յ���Ϣͳͳ�н��ս��̽��д���

4.����������ط�һ�ɲ��ٴ��������Ϣ�Ĵ���

5.��Ϣ���͵Ĺ����ǣ���ϢԴ��˭����Ϣ�͵��Ķ���ֹ����ֹ������Ϣ���������*********

*/
void *thread_recv(void *arg){
	int send_num;
	unsigned char send_buff[64];
	int recv_num;
	unsigned char recv_buff[64];
	
	unsigned char modbusorder[8]; // ���͸���λ�豸��modbus����
	unsigned char recv_device[64]; // ������λ�豸���ص�modbus��Ϣ
	
	int isModbusoederExist ;
	
	while(isConnectServer){
		printf("\n"); // ��һ�����У�����һ��ѭ�����̼������
		printf("recv_Thread: begin to recv Msg from server...\n");
		memset(recv_buff,0,sizeof(recv_buff));
		recv_num=recv(sock_fd,recv_buff,sizeof(recv_buff),0); //-------------------recv
		if(recv_num < 0){
			perror("recv_Thread: recv Msg failed\n");
			exit(1);
		}else{
			printf("recv_Thread: recv Msg successfully\n");
		}
		
		// �жϽ��յ���������,��һ���ֽڣ����ݲ�ͬ�����ͽ��в�ͬ�Ĵ���
		switch(recv_buff[0]){
			case 0x01: // �����������͸�server�������Լ�����ݣ�����Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x01\n");
				break;
				
			case 0x04: // ���ڴ򿪳ɹ���Ϣ������Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x04\n");
				break;
				
			case 0x05: // ���ڴ�ʧ����Ϣ������Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x05\n");
				break;
				
			case 0x07: // server����modbus�����modbus�ն���Ϣ����Ҫ���أ���Ϊ������Ϣ��Դ�ǡ�server����������
				printf("recv_Thread: the type is 0x07\n");
				
				// ��װ���ص���Ϣ��˵���Ѿ��յ�������
				send_buff[0] = 0x07;
				send_buff[1] = recv_buff[1];
				send_num=send(sock_fd,recv_buff,2,0); //-------------------send
				
				// ��modbus������뻺����
				int i,result;
				for(i=0;i< MODBUSORDERLENGTH ;i++){ // ��ȡmodbus����
					modbusorder[i]=recv_buff[i+1];
					//printf("%x ",modbusorder[i]);
				}
				//printf("\n");
				// ����-���ݿ�
				pthread_mutex_lock(&mutex_db);
				
				//�����ж����ݿ����Ƿ��Ѿ���������modbus����
				sqlite3_stmt * stat;
				sqlite3_prepare( db, "select * from modbusorder where data = ?;", -1, &stat, 0 );
				sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdataΪ���ݻ�������length_of_data_in_bytesΪ���ݴ�С�����ֽ�Ϊ��λ
				if((result = sqlite3_step( stat )) != SQLITE_ROW){ // ˵��û�ҵ���������
					sqlite3_finalize( stat ); //�Ѹղŷ��������������
					
					// �����µ�����
					sqlite3_prepare( db, "insert into modbusorder values(?);", -1, &stat, 0 );
					sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdataΪ���ݻ�������length_of_data_in_bytesΪ���ݴ�С�����ֽ�Ϊ��λ
					result = sqlite3_step( stat );
					sqlite3_finalize( stat ); //�Ѹղŷ��������������
				}
				
				// ����-���ݿ�
				pthread_mutex_unlock(&mutex_db);
				break;
				
			default:
				printf("recv_Thread: %x,this type can not be recognized!\n",recv_buff[0]);
				break;
		}
	}
}




/*---�̣߳������ݿ��ж�ȡmodbus�������modbus�����͸�server���߻��浽����

1.���ñ�ʶ������־�Ƿ��Ѿ��������ݿ�

2.���server�����ߣ���ֱ�ӻ���modbusdata�����ݿ���

3.���server���ߣ���ֱ�ӽ����ݷ��ظ�server������ʼ��ѯ���ݿ⣬������������ݣ�ͳͳ���ظ�server,����һ��ɾ��һ��

*/
void *thread_modbusdata(void *arg){
	int send_num;
	unsigned char send_buff[64];
	int recv_num;
	unsigned char recv_buff[64];
	//unsigned char send_device[8]; // ���͸���λ�豸��modbus����
	unsigned char recv_device[64]; // ������λ�豸���ص�modbus��Ϣ
	//char *recv_d = send_device;
	
	
	printf("modbusdata_Thread: the thread is beginning...\n");
	while(1){
		
		/* ���ȴ�modbusorder�ж�ȡmodbus���� */
		// ����-���ݿ�
		pthread_mutex_lock(&mutex_db);
		
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusorder;", -1, &stat, 0 );
		int result,j ;
		
		while((result = sqlite3_step( stat )) == SQLITE_ROW){
			unsigned char *recv_d = (unsigned char *)sqlite3_column_blob( stat, 0 ); // ָ�븳ֵ��ʱ�򣬸�ָ��ָ��������һƬ����
			int len = sqlite3_column_bytes( stat, 0 );
			printf("modbusdata_Thread: the modbusorder is:%d,%s\n",len,recv_d);
			/*
			for(j = 0;j < len;j++){
				printf("%x ",recv_d[j]);
			}
			printf("\n");
			*/
			// ��modbus��Ϣ���͸���λ�豸
			if(write(com_fd,recv_d,len) < 0 ){
				printf("modbusdata_Thread: send modbus order to device failed\n");
			}else{
				printf("modbusdata_Thread: send modbus order to device successfully\n");
			}
					
			// ����λ�豸��������
			printf("modbusdata_Thread: begin to recive modbus data from device...\n");
			int num_read;
			memset(recv_device,0,sizeof(recv_device));
					
			// �ж��Ƿ����λ�豸���յ����ݣ�ͨ���Ƿ��ܹ����յ������ж��豸�Ƿ�����
			if((num_read = read(com_fd,recv_device,64)) > 0){
				printf("modbusdata_Thread: recv modbus data from device successfully , Len:%d\n",num_read);

			}else{
				printf("modbusdata_Thread: recv modbus data from device failed, Len:%d\n",num_read);
				printf("modbusdata_Thread: recv modbus data from device failed\n");						
			}
			
			
		}
		printf("\n");
		sqlite3_finalize( stat ); //�Ѹղŷ��������������
		
		// ����-���ݿ�
		pthread_mutex_unlock(&mutex_db);
		
		sleep(SLEEPTIME);
	}
}










/**----------------------------------------------------------------------------------------------*/

/*---����modbus�豸�Ǳ�����������⣬�����ڴ���0x07���͵���Ϣ������

1.��modbus��������ȡ���豸�ţ�

2.�������λ�豸û�н��յ����ݣ���˵����λ�豸�����ߣ�

3.�������λ�豸���յ����ݣ���˵����λ�豸���ߣ�

4.����豸���ߡ����ߵ���Ϣ���͸�server�������ˣ�

*/


/*---����modbus��Ϣ���������

1.�ն˴�server���յ�modbus����

2.�ն˽�modbus������л���

3.���淽ʽ�������ļ��ķ�ʽ����

4.�ļ�������[modbus�����ļ�]
	1.�����ļ���д��ʶ������Ϊmodbus�����ļ����ܻ����
	2.ÿ�θ��µ�ʱ����Ҫ�Ƚ��ļ����
	3.�ļ���ȡ��ʱ�򣬰�˳���ȡ����

5.�ն˴�modbus�����ļ��а�˳���ȡ���ݣ�Ȼ���͸���λ�豸

6.�ն˽��մ���λ�豸���ص�����

7.�ն��ж�server�Ƿ�����
	1.������ߣ�������ֱ�ӷ��ظ�server
	2.��������ߣ������ݻ��棬���淽ʽ���ļ�
	
8.�ļ�������[modbus����]
	1.�����ļ���д��ʶ��������ͬʱ��д
	
	date -s 031413312016 //����ʱ��(����ʱ����)
*/

/**----------------------------------------------------------------------------------------------*/









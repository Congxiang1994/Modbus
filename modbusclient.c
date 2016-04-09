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
#include <time.h>
#include <linux/rtc.h>  

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


/* -��ʶ�������� */ 
int isOpenCom; // ��ʶ�Ƿ�򿪴���
int isConnectServer; // ��ʶ�Ƿ�����server����
int isGetModbusData; // ��ʶthread_modbusdata�߳��Ƿ���������
int isSendModbusdataToServer; // ��ʶ�Ƿ����������ݿ��е�modbusdata����server���������򡱵��߳�
int isGetModbusDataCanSendModbusdataToServer; // ��ʶthread_modbusdata�߳��Ƿ��ܹ���ʼ�������ݸ�server����Ϊthread_connectServer��Ҫ��һ�η�������

/* -�߳�ͬ�������ݿ��д */
pthread_mutex_t mutex_db; // ���ݿ⻥���ź�����һ��ֻ��һ���߳��ܹ��������ݿ�

/* -���������� */
void *thread_recv(void *arg); // �����ն���server����������֮�����Ϣͨ��
void *thread_connectServer(void *arg); // ����ѯ��server�����Ƿ�����
void *thread_modbusdata(void *arg); // ����modbus�ն�����λ�豸֮���ͨ�ţ��Լ�����������
void *thread_keyboard(void *arg); // ��������¼������ÿ�ݼ���ֹ�����������������Ĺرճ�����
void *thread_sendModbusdataToServer(void *arg); // �����ݿ��е����ݷ��ظ�server����������

int sendMsgToServer(volatile int sock_fd, unsigned char typeOfMsg, unsigned char *contentOfMsg, int lengthOfMsg);

void openCOM(); // �򿪴��ں���

int sqlcreate(char *sql, char *table, char *key, char *kind);
int sqlinsert(char *sql, char *table, char *key, char *value);
int sqldelete(char *sql, char *table, char *key, char *value);
int sqlselect(char *sql, char *table, char *key, char *value);
int select_callback(void *data, int n_columns, char **column_values, char **column_names);

/**---ctrl+c����Ϣ��Ӧ�¼�
1.�����������е��߳�

2.�����˳�����

*/
void Stop(int signo) 
{
	printf("Stop: Begin to end this program\n");

	isGetModbusData = FALSE; // ����thread_modbusdata�߳�
	isConnectServer = FALSE;
	isGetModbusDataCanSendModbusdataToServer = FALSE;
	
	// ����-���ݿ�
	////pthread_mutex_lock(&mutex_db);
	sqlite3_close(db);
	db=0;
	close(sock_fd);
	sleep(SLEEPTIME);
	printf("Stop: EXIT!!!\n");
	exit(0);
	
	// ����-���ݿ�
	////pthread_mutex_unlock(&mutex_db);
}

/**---������

1.�������ݿ⼰��

2.���ô��ڲ��������򿪴���

3.�����̣߳�����λ�豸�ɼ�����

4.�����̣߳�������server������������

*/
int main(){
	/* �ȱ�ʶδ�����Ϸ����� */
	isConnectServer=FALSE; 
	
	/* �òɼ���������̵߳�ѭ������ΪTRUE */
	isGetModbusData = TRUE;
	
	/* �òɼ���������̵߳ġ��ܷ������ݸ�server���ı�ʶ��ΪFALSE */
	isGetModbusDataCanSendModbusdataToServer = FALSE;
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* ��ʼ�����ݿ⻥���ź��� */
	int init = pthread_mutex_init(&mutex_db, NULL);
	if(init != 0)
	{
	   printf("main_Function: mutex_db init failed \n");
	   return 0;
	} 
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* �������ݿ⼰���ݿ�� */
	// 1.�������ݿ� 
	printf("main_Function: Begin to create a database...\n");
	res_db = sqlite3_open("modbus",&db);
	if(res_db != SQLITE_OK){
		fprintf(stderr, "�����ݿ�ʧ��: %s\n", sqlite3_errmsg(db));
		return 0;
	}
	
	// 2.�����ݿ��modbusorder 
	printf("main_Function:Begin to create table:modbusorder...\n");
	sqlcreate(sql,"modbusorder","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusorder error:%s\n", errMsg);
	}
	
	// 3.�����ݿ��modbusdata
	printf("main_Function:Begin to create table:modbusdata...\n");
	sqlcreate(sql,"modbusdata","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusdata error:%s\n", errMsg);
	}
	printf("main_Function:create database and table successfully.\n\n");
	sqlite3_close( db ); // �ر����ݿ�
	db=0;
	signal(SIGINT, Stop); 
	/**--------------------------------------------------------------------------------------------------------*/
	
	
	/* �򿪴��� */
	openCOM();
	/**--------------------------------------------------------------------------------------------------------*/
	
	
	/* ����һ���̣߳�����modbus�ն�����λ�豸֮���ͨ�ţ��Լ����������� */
	printf("connectServer_Thread: begin to create a thread [in order to deal with the data from device]......\n");
	pthread_t thread_modbusdataMsg;
	int res_modbusdata;
	res_modbusdata=pthread_create(&thread_modbusdataMsg,NULL,thread_modbusdata,NULL);
	if(res_modbusdata != 0){
		perror("connectServer_Thread: thread_modbusdata creat failed\n\n");
		exit(1);
	}else{
		printf("connectServer_Thread: thread_modbusdata create sucessfully\n\n");
	}
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* ����һ���̣߳�������������server���������� */
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
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* �����ѭ���������Ǳ������̲߳��رգ��������߳̾���һֱ���� */
	while(1){
		sleep(10000);
	}

}

/**---�̣߳���������server������

1.��ͣ��ѯ��server�������Ƿ�����

2.����server�����������Լ�modbus�ն˵����

3.����server���������򣬴����Ƿ�򿪳ɹ�

4.�����̣߳�����λ�豸�ɼ�����

*/
void *thread_connectServer(void *arg){
	
	/* ����server���������� */
	// 1.�����׽���
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
	
	// 2.����ѭ�����ȴ�server��������
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
	/**--------------------------------------------------------------------------------------------------------*/
	
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
	/**--------------------------------------------------------------------------------------------------------*/
	
	sleep(SLEEPTIME); // ��ͣһ�£��ȴ������߳������ɹ�
	
	/* �����������͸�server�������Լ������ */
	unsigned char typeOfMsg ; // ��Ϣ����
	int send_num; // ��¼���͵��ֽ���
	unsigned char send_buff[64]; // ���ͻ�����
	int recv_num; // ��¼���յ��ֽ���
	unsigned char recv_buff[64]; // ���ջ�����
	
	printf("connectServer_Thread: begin to send type [identity myself: 0x01]......\n");
	/*
	send_buff[0]=0x01; // ���������01
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	*/
	typeOfMsg = 0x01;
	if(sendMsgToServer(sock_fd, typeOfMsg, NULL, 0) == 1){
		printf("connectServer_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
	}else{
		printf("connectServer_Thread: send type failed:0x%02x\n\n",typeOfMsg);
		exit(1);
	}
	
	/**--------------------------------------------------------------------------------------------------------*/

	sleep(SLEEPTIME); // ��ͣһ��
	/* �������Ƿ�ɹ��򿪵���Ϣ����server���������� */
	if(isOpenCom == TRUE){
		typeOfMsg=0x04; // �򿪴��ڼ����ò����ɹ�
	}else{
		typeOfMsg=0x05; // �򿪴��ڼ����ò���ʧ��
	}
	printf("connectServer_Thread: begin send type [open COM state]......\n");
	/*
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	*/
	if(sendMsgToServer(sock_fd, typeOfMsg, NULL, 0) == 1 ){
		printf("connectServer_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
	}else{
		printf("connectServer_Thread: send type failed:0x%02x\n\n",typeOfMsg);
		exit(1);
	}
	
	sleep(SLEEPTIME); // ��ͣһ��
	isGetModbusDataCanSendModbusdataToServer = TRUE;
	/**--------------------------------------------------------------------------------------------------------*/
	

	/* ����һ�������ݿ��еļ�����ݷ��ظ�server���߳� */
	printf("connectServer_Thread: begin to create a thread [in order to send modbusdata to server]......\n");
	pthread_t thread_sendDataToServer;
	int res_sendDataToServer;
	res_sendDataToServer=pthread_create(&thread_sendDataToServer,NULL,thread_sendModbusdataToServer,NULL);
	if(res_sendDataToServer != 0){
		perror("connectServer_Thread: thread_sendDataToServer creat failed\n\n");
		exit(1);
	}else{
		printf("connectServer_Thread: thread_sendDataToServer create sucessfully\n\n");
	}	
	/**--------------------------------------------------------------------------------------------------------*/
}


/**---�����ն���server����������֮�����Ϣͨ��

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
	
	unsigned char typeOfMsg ; // ���͵���Ϣ����
	
	unsigned char modbusorder[8]; // ���͸���λ�豸��modbus����
	unsigned char recv_device[64]; // ������λ�豸���ص�modbus��Ϣ
	
	char currentTime[64]; // �����洢server���͹�����ϵͳʱ��
	
	int isModbusoederExist ;
	
	while(isConnectServer){
		printf("\n"); // ��һ�����У�����һ��ѭ�����̼������
		printf("recv_Thread: begin to recv Msg from server...\n");
		memset(recv_buff,0,sizeof(recv_buff));
		
		/*
		recv_num=recv(sock_fd,recv_buff,sizeof(recv_buff),0); //-------------------recv
		*/
		recv_num = recvMsgFromServer(sock_fd, recv_buff, sizeof(recv_buff)); // �����Լ���д��recv����
		
		if(recv_num < 0){
			printf("recv_Thread: recv Msg failed\n");
			isConnectServer = FALSE;
			isGetModbusDataCanSendModbusdataToServer = FALSE;
			/* ������Ҫ������������server������������̣߳����������� */
			printf("recv_Thread: begin to REconnect to server......\n");
			pthread_t thread_connectToServer;
			int res_connectToServer;
			res_connectToServer=pthread_create(&thread_connectToServer,NULL,thread_connectServer,NULL);
			if(res_connectToServer != 0){
				perror("recv_Thread: thread_connectToServer creat failed\n\n");
				exit(1);
			}else{
				printf("recv_Thread: thread_connectToServer create sucessfully\n\n");
			}
			
			return 0; // ����߳̿��Է��ؽ�����
			//exit(1);
		}else{
			printf("recv_Thread: recv Msg successfully\n");
		}
		
		// �жϽ��յ���������,��һ���ֽڣ����ݲ�ͬ�����ͽ��в�ͬ�Ĵ���
		switch(recv_buff[0]){
			case 0x01: // �����������͸�server�������Լ�����ݣ�����Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x01\n");
				break;
				
			case 0x02: // ������λ�豸���ߵ���Ϣ���͸�server������Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x02\n");
				break;	
	
			case 0x03: // ������λ�豸���ߵ���Ϣ���͸�server������Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x03\n");
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
				/*
				send_buff[0] = 0x07;
				send_buff[1] = recv_buff[1];
				send_num=send(sock_fd,recv_buff,2,0); //-------------------send
				*/
				typeOfMsg = 0x07;
				if(sendMsgToServer(sock_fd, typeOfMsg, &recv_buff[1], 1) == 1 ){
					printf("recv_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
				}else{
					printf("recv_Thread: send type failed:0x%02x\n\n",typeOfMsg);
					//exit(1);
					break;
				}
				
				// ��modbus������뻺����
				int i,result;
				for(i=0;i< MODBUSORDERLENGTH ;i++){ // ��ȡmodbus����
					modbusorder[i]=recv_buff[i+1];
					//printf("%x ",modbusorder[i]);
				}
				//printf("\n");
				// ����-���ݿ�
				pthread_mutex_lock(&mutex_db);
				res_db = sqlite3_open("modbus",&db);
				// 1.�����ж����ݿ����Ƿ��Ѿ���������modbus����
				sqlite3_stmt * stat;
				sqlite3_prepare( db, "select * from modbusorder where data = ?;", -1, &stat, 0 );
				sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdataΪ���ݻ�������length_of_data_in_bytesΪ���ݴ�С�����ֽ�Ϊ��λ
				result = sqlite3_step( stat );
				sqlite3_finalize( stat ); // �Ѹղŷ��������������
				if(result != SQLITE_ROW){ // ˵��û�ҵ���������
					// 2.�����µ�����
					sqlite3_prepare( db, "insert into modbusorder values(?);", -1, &stat, 0 );
					sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdataΪ���ݻ�������length_of_data_in_bytesΪ���ݴ�С�����ֽ�Ϊ��λ
					result = sqlite3_step( stat );
					printf("recv_Thread: the result of sqlite3_step is:%d\n",result);
					sqlite3_finalize( stat ); //�Ѹղŷ��������������
				}
				sqlite3_close(db);
				db=0;
				// ����-���ݿ�
				pthread_mutex_unlock(&mutex_db);
				break;
				
			case 0x09: // server���͵�ǰϵͳʱ���modbus�ն���Ϣ����Ҫ���أ���Ϊ������Ϣ��Դ�ǡ�server����������
				printf("recv_Thread: the type is 0x09\n");
				/*
				send_buff[0] = 0x09;
				send_buff[1] = recv_buff[1];
				send_num=send(sock_fd,recv_buff,2,0); //-------------------send
				*/
				typeOfMsg = 0x09;
				if(sendMsgToServer(sock_fd, typeOfMsg, &recv_buff[1], 1) == 1 ){
					printf("recv_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
				}else{
					printf("recv_Thread: send type failed:0x%02x\n\n",typeOfMsg);
					//exit(1);
					break;
				}
				
				// 1.��ȡ��server���͹�����ʱ��
				printf("recv_Thread: Begin to get the current time\n");
				int k;
				for(k = 1; k< recv_num; k++){
					currentTime[k-1] = recv_buff[k];
				}
				printf("recv_Thread: the currentTime is:%s\n",currentTime);
				
				// 2.����ϵͳʱ��
				printf("recv_Thread: Begin to update system time\n");
				if(SetSystemTime(currentTime) == 0){
					printf("recv_Thread: Update system time successfully\n");
				}else{
					printf("recv_Thread: Update system time failed\n");					
				}
				break;
				
			case 0x0A: // modbus�ն˷��ؼ�����ݸ�server��������Ϣ������Ҫ���أ���ֹ��Ϣ������������Ϊ������Ϣ��Դ�ǡ�modbus�նˡ�
				printf("recv_Thread: the type is 0x0A\n");
				break;
				
			default:
				printf("recv_Thread: %x,this type can not be recognized!\n",recv_buff[0]);
				//exit(1); // ���ִ������˳�
				break;
		}
	}
}




/**---�̣߳�����λ�豸�ɼ�modbusdata�������

1.�����ݿ��л�ȡmodbusorder����

2.�����modbusdata��Ϊ�ա����ߡ�δ����server���������򡱣���modbusdata���浽�������ݿ���

3.�����modbusdataΪ�ա����ҡ�������server���������򡱣���modbusdataֱ�ӷ��͸�server������

*/
void *thread_modbusdata(void *arg){
	int send_num; // ���͸�server������������ֽ�����
	unsigned char recv_device[64]; // ������λ�豸���ص�modbus��Ϣ
	unsigned char modbus_buff[128]; // ���ݻ��壺��ǰϵͳʱ��+modbus����
	unsigned char typeOfMsg ; // ���͵���Ϣ����
	
	unsigned char isDeviceOnOrOff; // ��ʶ��λ�豸�����߻�������
	
	printf("modbusdata_Thread: the thread is beginning...\n");
	while(isGetModbusData){
		
		/* ��modbusorder�ж�ȡmodbus���� */
		// ����-���ݿ�
		pthread_mutex_lock(&mutex_db);
		res_db = sqlite3_open("modbus",&db);
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusorder;", -1, &stat, 0 );
		int result,j,k,s,len; // len:modbus����ĳ�����ͬ
		unsigned char recv_d[32][8]={0}; // �������32������
		k = 0;
		while((result = sqlite3_step( stat )) == SQLITE_ROW){
			memcpy(recv_d[k],sqlite3_column_blob( stat, 0 ),8);
			//recv_d[k] = (unsigned char *)sqlite3_column_blob( stat, 0 ); // ָ�븳ֵ��ʱ�򣬸�ָ��ָ��������һƬ����
			len = sqlite3_column_bytes( stat, 0 );
			printf("modbusdata_Thread: !!!!!!the No.%d modbusorder is:%d,%s\n",k,len,recv_d[k]);
			k++;
		}
		sqlite3_finalize( stat ); //�Ѹղŷ��������������
		printf("modbusdata_Thread: the num of modbusorder is:%d\n",k);
		sqlite3_close(db);
		db=0;
		// ����-���ݿ�
		pthread_mutex_unlock(&mutex_db);

		for(s = 0;s < k; s++){
			printf("modbusdata_Thread: No.%d modbus order is:%s\n",s,recv_d[s]);
		}
		
		for(s = 0;s < k; s++){
			//////////////////sqlite3_finalize( stat ); //�Ѹղŷ��������������
			printf("modbusdata_Thread: Begin to send No.%d modbus order to device:%s\n",s,recv_d[s]);
			// ��modbus��Ϣ���͸���λ�豸
			if(write(com_fd,recv_d[s],len) < 0 ){
				printf("modbusdata_Thread: send modbus order to device failed\n");
			}else{
				printf("modbusdata_Thread: send modbus order to device successfully\n");
			}
					
			// ����λ�豸��������
			printf("modbusdata_Thread: begin to recive modbus data from device...\n");
			int num_read;
			memset(recv_device,0,sizeof(recv_device));
					
			/* ����λ�豸״̬�ı�ʶ����ʼ������ΪFalse */
			isDeviceOnOrOff = 0x03;
			
			// �ж��Ƿ����λ�豸���յ����ݣ�ͨ���Ƿ��ܹ����յ������ж��豸�Ƿ�����
			if((num_read = read(com_fd,recv_device,64)) > 0){
				
				/* ��ʶ����λ�豸�����߻������� */
				isDeviceOnOrOff = 0x02; // ��λ�豸���ߣ���ʶ����ΪTRUE
				
				int i;
				printf("modbusdata_Thread: recv modbus data from device successfully , Len:%d\n",num_read);
				for(i = 0;i < num_read ;i++ ){
					printf("%x ",recv_device[i]);
				}
				printf("\n");
				
				/*��װmodbusdata���ݣ�ʱ�� + data*/
				unsigned char str_time[19];
				getSystemTime(str_time);
				printf("modbusdata_Thread: the currentTime is:%s\n",str_time);
				
				memset(modbus_buff,0,sizeof(modbus_buff));
				strcat(modbus_buff,str_time); // ��ʱ����ӵ�modbus_buff��
				
				// ע�⣺modbus������16������������ֱ�Ӱ��մ����ַ����ķ�ʽֱ�ӽ��ַ�����������,��Ҫͨ��һ��ѭ������������
				for(i = 0; i < num_read; i++){
					modbus_buff[19+i] = recv_device[i];
				}
				
				printf("modbusdata_Thread: the modbusdata is:");
				for(i = 19;i < num_read + 19;i++ ){
					printf("%x ",modbus_buff[i]);
				}
				printf("\n");
				// ------���ˣ���װ��ʱ�� + data���ɹ���modbus_buff�ĳ���Ϊ��19+num_read
				
				
				/* ����ʵ���������modbus�ն˲ɼ������ݽ��д��� */
				// 1.�����modbusdata��Ϊ�ա����ߡ�δ����server���������򡱣���modbusdata���浽�������ݿ���
				// 2.�����modbusdataΪ�ա����ҡ�������server���������򡱣���modbusdataֱ�ӷ��͸�server������
				// ����-���ݿ�
				pthread_mutex_lock(&mutex_db);
				res_db = sqlite3_open("modbus",&db);
				sqlite3_stmt * stat_data;
				sqlite3_prepare( db, "select * from modbusdata;", -1, &stat_data, 0 );
				int result_data = sqlite3_step( stat_data );
				sqlite3_finalize( stat_data ); //�Ѹղŷ��������������
				// ����-���ݿ�
				pthread_mutex_unlock(&mutex_db);
							
				if( result_data == SQLITE_ROW || isGetModbusDataCanSendModbusdataToServer == FALSE){ // ���ݿ��д��ڼ������ || δ����server�����������򽫼�����ݲ������ݿ�modbusdata��
					
					printf("modbusdata_Thread: the table modbusdata is not empty or has not connected the server .\n");
					
					
					// ������ֱ�ӷŽ����ݿ���,
					//result = sqlite3_exec( db, "begin transaction", 0, 0, &errMsg ); // ��ʼһ������					
					// ����-���ݿ�
					pthread_mutex_lock(&mutex_db);
					sqlite3_prepare( db, "insert into modbusdata values(?);", -1, &stat_data, 0 );
					sqlite3_bind_blob( stat_data, 1, modbus_buff, num_read + 19 , NULL ); // pdataΪ���ݻ�������length_of_data_in_bytesΪ���ݴ�С�����ֽ�Ϊ��λ
					result = sqlite3_step( stat_data );
					printf("modbusdata_Thread: the result of sqlite3_step is:%d\n",result);
					sqlite3_finalize( stat_data ); //�Ѹղŷ��������������
					//result = sqlite3_exec( db, "commit transaction", 0, 0, &errMsg ); // �ύ����
					sqlite3_close(db);
					db=0;
					// ����-���ݿ�
					pthread_mutex_unlock(&mutex_db);

				}else{ // ���ݿ��в����ڼ������ && �Ѿ�����server�����������򽫼������ֱ�ӷ��ظ�server����������
					printf("modbusdata_Thread: the table modbusdata is empty and has connected the server.\n");
/*
					// ��modbus��Ϣ��������Ųһλ���ڵ�һλ����0x0A
					for(i = num_read+19; i > 0; i--){ 
						modbus_buff[i+1]=modbus_buff[i];
					}
					modbus_buff[0] = 0x0A;
					
					// ����Ϣ���ظ�server����������
					printf("modbusdata_Thread: begin to return modbus data to server, the type is 0x0A...\n");
					send_num=send(sock_fd,modbus_buff,num_read+19+1,0);//-------------------send
*/
					typeOfMsg = 0x0A;
					if(sendMsgToServer(sock_fd, typeOfMsg, modbus_buff, num_read+19) == 1 ){
						printf("modbusdata_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
					}else{
						printf("modbusdata_Thread: send type failed:0x%02x\n\n",typeOfMsg);
						//exit(1);
						continue;
					}
				}
			}else{
				printf("modbusdata_Thread: recv modbus data from device failed, Len:%d\n",num_read);
				printf("modbusdata_Thread: recv modbus data from device failed\n");						
			}
			
			/* ����λ�豸�Ƿ����ߵ���Ϣ���͸���λ�� */
			// 1.�ж��Ƿ��ܹ���server��������
			if(isGetModbusDataCanSendModbusdataToServer == TRUE){
				if(sendMsgToServer(sock_fd, isDeviceOnOrOff, recv_d[s], 1)){ // ֻ��Ҫ����λ�豸ID���͹�ȥ����
					printf("modbusdata_Thread: send type successfully:0x%02x\n\n",isDeviceOnOrOff);
				}else{
					printf("modbusdata_Thread: send type failed:0x%02x\n\n",isDeviceOnOrOff);
					//exit(1);
					continue;
				}
			}
		}
		printf("\n");
		sleep(SLEEPTIME);
	}
	printf("modbusdata_Thread: the thread itself is closed successfully\n\n");
}

/**---�̣߳������ݿ��еļ�����ݷ��ظ�server����

1.ǰ�᣺������server����������

2.ÿ�δ����ݿ���ȡһ����õ�modbusdata������modbusdata���͸�server����������

3.�ɹ��������ݺ�ɾ������modbusdata

*/
void *thread_sendModbusdataToServer(void *arg){
	
	unsigned char modbus_buff[128]; // ���ݻ��壺��ǰϵͳʱ��+modbus����
	
	int send_num; // ���͸�server������������ֽ�����
	
	unsigned char typeOfMsg ; // ���͵���Ϣ����
	
	printf("sendModbusdataToServer_Thread: the thread is beginning...\n");
	
	while(isConnectServer){
		// ����-���ݿ�
		pthread_mutex_lock(&mutex_db);
		
		memset(modbus_buff,0,sizeof(modbus_buff));
		
		res_db = sqlite3_open("modbus",&db);
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusdata;", -1, &stat, 0 );
		int result,len,i; // len:modbusdata�ĳ���
		 //
		result = sqlite3_step( stat );
		//sqlite3_finalize( stat ); //�Ѹղŷ��������������
		unsigned char recv_d[64]={'\0'};
		if(result == SQLITE_ROW){
			printf("sendModbusdataToServer_Thread: the modbusdata is not empty.\n");
			len = sqlite3_column_bytes( stat, 0 );
			memcpy(recv_d,sqlite3_column_blob( stat, 0 ),len);
			//unsigned char *recv_d = (unsigned char *)sqlite3_column_blob( stat, 0 );
			
			sqlite3_finalize( stat ); //�Ѹղŷ��������������
			printf("sendModbusdataToServer_Thread: the modbusorder is:%d,%s\n",len,recv_d);
			
			/*
			// ��modbus��Ϣ��������Ųһλ���ڵ�һλ����0x0A
			for(i = len; i > 0; i--){ 
				modbus_buff[i+1]=recv_d[i];
			}
			modbus_buff[0] = 0x0A;	
			// ����Ϣ���ظ�server����������
			printf("sendModbusdataToServer_Thread: begin to return modbus data to server, the type is 0x0A...\n");
			send_num=send(sock_fd,modbus_buff,len+1,0);//-------------------send
			*/
			typeOfMsg = 0x0A;
			if(sendMsgToServer(sock_fd, typeOfMsg, recv_d, len) == 1 ){
				printf("sendModbusdataToServer_Thread: send type successfully:0x%02x\n\n",typeOfMsg);
			}else{
				printf("sendModbusdataToServer_Thread: send type failed:0x%02x\n\n",typeOfMsg);
				//exit(1);
				break;
			
			}
			// ������modbusdataɾ��
			printf("sendModbusdataToServer_Thread: begin to delete the modbusdata\n");
			sqlite3_prepare( db, "delete from modbusdata where data = ? ;", -1, &stat, 0 );
			sqlite3_bind_blob( stat, 1, recv_d, len , NULL ); 
			result = sqlite3_step( stat );
			printf("sendModbusdataToServer_Thread: the result of sqlite3_step is:%d\n",result);
			sqlite3_finalize( stat ); //�Ѹղŷ��������������
			printf("sendModbusdataToServer_Thread: delete the modbus data:%s\n",recv_d);
		}else{
			sqlite3_finalize( stat ); //�Ѹղŷ��������������
		}
		
		// ����-���ݿ�
		pthread_mutex_unlock(&mutex_db);
		
		sqlite3_close(db);
		db=0;
		
		sleep(1);		
	}
}
/************************************************ 
����������modbus�ն��ϵĴ��ڲ������򿪴���
**************************************************/  
void openCOM(){
	/* ��ʼ׼���򿪴��� */
	printf("openCOM: begin to open COM......\n");
	// 1.���ô������� 
	struct termios oldtio,newtio;
	com_fd = open(COM2,O_RDWR);
	int isopen = TRUE;
	if(com_fd < 0){
		printf("openCOM: open COM2 failed\n");
		isopen = FALSE;
		//exit(1);
	}else{
		printf("openCOM: open COM2 successfully\n");
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
			printf("openCOM: set COM2 attribute failed\n");
			isopen = FALSE;
			//exit(1);
		}else{
			printf("openCOM: set COM2 attribute successfully\n\n");
		}	
	}

	// 2.���ݱ�ʶ����������ʲô�ַ���server
	if(isopen == TRUE){
		isOpenCom = TRUE;
		printf("openCOM: open COM state successfully\n\n");
	}else{
		isOpenCom = FALSE;
		printf("openCOM: open COM state failed\n\n");
	}
}

/************************************************ 
���ò���ϵͳʱ�� 
����:*dt���ݸ�ʽΪ"2006-4-20 20:30:30" 
���÷���: 
    char *pt="2006-4-20 20:30:30"; 
    SetSystemTime(pt); 
**************************************************/  
int SetSystemTime(char *dt)  
{  
    struct rtc_time tm;  
    struct tm _tm;  
    struct timeval tv;  
    time_t timep;  
    sscanf(dt, "%d-%d-%d %d:%d:%d", &tm.tm_year,  
        &tm.tm_mon, &tm.tm_mday,&tm.tm_hour,  
        &tm.tm_min, &tm.tm_sec);  
    _tm.tm_sec = tm.tm_sec;  
    _tm.tm_min = tm.tm_min;  
    _tm.tm_hour = tm.tm_hour;  
    _tm.tm_mday = tm.tm_mday;  
    _tm.tm_mon = tm.tm_mon - 1;  
    _tm.tm_year = tm.tm_year - 1900;  
  
    timep = mktime(&_tm);  
    tv.tv_sec = timep;  
    tv.tv_usec = 0;  
    if(settimeofday (&tv, (struct timezone *) 0) < 0)  
    {  
    printf("Set system datatime error!/n");  
    return -1;  
    }  
    return 0;  
}  

/************************************************ 
��ȡ����ϵͳʱ�� 
��ʽ��"2006-04-20 20:30:30"; ����Ҫһ�� ����Ϊ19
sprintf(char *string, char *format,arg1,arg2...);
%02d:����Ļ�����0��
**************************************************/ 
int getSystemTime(char *strtime)   
{   
	time_t timer;   
	struct tm* t_tm;   
	time(&timer);   
	t_tm = localtime(&timer);   
	//printf("today is %4d%02d%02d%02d%02d%02d/n", t_tm.tm_year+1900, t_tm.tm_mon+1, t_tm.tm_mday, t_tm.tm_hour, t_tm.tm_min, t_tm.tm_sec);   
	sprintf(strtime, "%4d-%02d-%02d %02d:%02d:%02d",t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec);
	return 0;   
}  
/**----------------------------------------------------------------------------------------------*/
/**---������������Ϣ��server����������
������
1.volatile int sock_fd; // �׽��ֵ��ļ�������
2.char *typeOfMsg; // ��Ϣ����
3.char *contentOfMsg; // ��Ϣ����
4.int lengthOfMsg; // ��Ϣ���ݵĳ���(��������Ϣ����)
����ֵ��
1:�ɹ���������
0:��������ʧ��
*/
int sendMsgToServer(volatile int sock_fd, unsigned char typeOfMsg, unsigned char *contentOfMsg, int lengthOfMsg){
/*	
	// 1.���������ݵĻ�����������ʼ��Ϊ��
	int send_num; // ��¼���͵��ֽ���
	unsigned char send_buff[128]; 
	memset(send_buff,0,sizeof(send_buff)); // ��ʼ��Ϊ��
	
	// 2.�����Ϣ����
	send_buff[0]=typeOfMsg; // ��Ϣ���;�һ���ֽ�
	printf("sendMsgToServer: type:0x%02x\n",send_buff[0]);
	
	// 3.�����Ϣ����
	int i;
	for(i = 0;i < lengthOfMsg; i++){
		send_buff[i+1] = contentOfMsg[i];
	}
	printf("sendMsgToServer:Begin to  send Msg\n");
	
	// 4.��������
	send_num = send(sock_fd,send_buff,lengthOfMsg+1,0); //-------------------send type
	if(send_num < 0){
		printf("sendMsgToServer: send type failed:%x\n\n",send_buff[0]);
		return 0; // ����ʧ��
	}else{
		printf("sendMsgToServer: send type successfully:%x\n\n",send_buff[0]);
		return 1; // ���ͳɹ�
	}
*/	
	/***-------------------------------------------------------------------------------*/
	/* ��д�������ݵĺ��� */
	// 1.���巢�ͻ��������������ĳ���Ϊ��lengthOfMsg + 1 + 1 + 1�������lengthOfMsg��������Ϣ�����룩
	int send_num; // ��¼���͵��ֽ���
	int i;
	unsigned char send_buff[128];
	memset(send_buff,0,sizeof(send_buff)); // ��ʼ��Ϊ��
	
	// 2.�����ݻ���������ӡ���Ϣ�峤�ȡ��ֶΣ�1���ֽڣ���16��������ʾ
	send_buff[0] = (char)(lengthOfMsg + 1); // �����lengthOfMsg�ǲ�������Ϣ���͵�
	
	// 3.�����ݻ���������ӡ���Ϣ������ + ��Ϣ�塱��lengthOfMsg + 1���ֽ�
	send_buff[1] = typeOfMsg;
	printf("sendMsgToServer: type:0x%02x\n",send_buff[1]);
	for(i = 0;i < lengthOfMsg; i++){
		send_buff[i+2] = contentOfMsg[i];
	}

	// 4.�����ݻ���������ӡ�ֹͣ���š���1���ֽ�
	send_buff[lengthOfMsg+2] = 0xFF;
	
	// 5.���ͻ��������������ݣ���Ϣ���� + ��Ϣ������ + ��Ϣ�� + ֹͣ����0xFF
	printf("sendMsgToServer:Begin to send Msg\n");
	send_num = send(sock_fd,send_buff,lengthOfMsg + 3,0); //-------------------send type
	if(send_num < 0){
		printf("sendMsgToServer: send type failed:%x\n\n",send_buff[1]);
		return 0; // ����ʧ��
	}else{
		printf("sendMsgToServer: send type successfully:%x\n\n",send_buff[1]);
		return 1; // ���ͳɹ�
	}
}
/**---���������մ�server���������͵���Ϣ(���recv�ĺ����������߳�)
������
1.volatile int sock_fd; // �׽��ֵ��ļ�������
2.unsigned char *recv_buff; // ���ջ�����
3.int lengthOfRecvBuff; // ���ջ������Ĵ�С
����ֵ��
recv_num
*/
int recvMsgFromServer(volatile int sock_fd, unsigned char *recv_buff, int lengthOfRecvBuff){
/*	
	int recv_num; // ���յ��ֽ���
	memset(recv_buff,0,strlen(recv_buff)); // �����������
	
	recv_num=recv(sock_fd, recv_buff, lengthOfRecvBuff, 0); //-------------------recv
	return recv_num;
*/
	/***-------------------------------------------------------------------------------*/
	/* ��д�������ݵĺ��� */
	unsigned char byteTemp[1];
	int recv_num;
	// 1.����һ���ַ�����Ϣ��ĳ���lengthOfMsg
	memset(byteTemp,0,strlen(byteTemp)); // �����������
	recv(sock_fd, byteTemp, 1, 0); //-------------------recv
	int lengthOfMsg = (int) byteTemp[0];
	
	// 2.����lengthOfMsg���ַ�
	recv_num = recv(sock_fd, recv_buff, lengthOfMsg, 0); //-------------------recv
	
	// 3.�ٶ���һ���ַ����ж��ǲ��ǡ�ֹͣ����0xFF��
	memset(byteTemp,0,strlen(byteTemp)); // �����������
	recv(sock_fd, byteTemp, 1, 0); //-------------------recv
	
	if(byteTemp[0] == 0xFF){
		// 4.�����ֹͣ���ţ�����recvNum == lengthOfMsg ��˵��������ȷ
		return recv_num;
	}else{
		// 5.�������ֹͣ���ţ�����recvNum != lengthOfMsg����˵�����մ���>>>���մ�����:��ͣ�Ķ�����һ���ַ���֪������0xFFΪֹ
		while(recv(sock_fd, byteTemp, 1, 0)){
			if(byteTemp[0] == 0xFF){
				break;
			}
		}
		return -1;
	}

}


/*
unsigned char recv_buff[64];
		memset(recv_buff,0,sizeof(recv_buff));
		recv_num=recv(sock_fd,recv_buff,sizeof(recv_buff),0); //-------------------recv

*/

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
/*
��ѯ16���Ƶ���
select hex(data) from modbusorder;
1 3 14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
1 3 14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0

01 03 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 a3 67 00
*/
/*---���ڵ����⣺
1.��ctrl+c��ֹ������ٴ���������ʱ��sqlite3������һ��-journal���ļ��������޷�������������

2.sqlite3_finalize( stat_data ); //�Ѹղŷ��������������������������������������Ҫ��ԣ�����ȱ�٣���Ȼ�ᵼ�´���
*/

/*---���ڵ����⣺
1.thread_modbusdata����ǰһ�����͹������server������������server������������ȷʶ����ʲô�豸����������

*/






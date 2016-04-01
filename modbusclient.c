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

#define SLEEPTIME 3 // sleep();间隔时间，单位是秒
#define MAXLINE 128 // 最长字符串
#define SQLLENGTH 256 // sql语句的长度
#define MODBUSORDERLENGTH 8 // modbus命令的长度

/* -全局变量定义区 */
volatile int com_fd; // 串口的文件描述符
volatile int sock_fd; // 套接字的文件描述符

sqlite3 *db; // 数据库文件指针
int res_db; // 用来判断sqlite函数是否执行正确
char sql[SQLLENGTH] = "\0" ;
char *errMsg; // 用来执行sqlite3执行的错误信息
unsigned char ** dbResult; //是 char ** 类型，两个*号
int nRow, nColumn;


/* -标识量定义区 */ 
int isOpenCom; // 标识是否打开串口
int isConnectServer; // 标识是否连接server程序

/* -线程同步：数据库读写 */
pthread_mutex_t mutex_db; // 数据库互斥信号量，一次只有一个线程能够访问数据库

/* -函数声明区 */
void *thread_recv(void *arg); // 处理终端与server服务器程序之间的消息通信
void *thread_connectServer(void *arg); // 处理询问server程序是否上线
void *thread_modbusdata(void *arg); // 处理modbus终端与下位设备之间的通信，以及处理监测数据

int sqlcreate(char *sql, char *table, char *key, char *kind);
int sqlinsert(char *sql, char *table, char *key, char *value);
int sqldelete(char *sql, char *table, char *key, char *value);
int sqlselect(char *sql, char *table, char *key, char *value);
int select_callback(void *data, int n_columns, char **column_values, char **column_names);


int main(){
	
	/* 初始化数据库互斥信号量 */
	int init = pthread_mutex_init(&mutex_db, NULL);
	if(init != 0)
	{
	   printf("main_Function:mutex_db init failed \n");
	   return 0;
	} 

	/* 建立数据库 */
	printf("main_Function:Begin to create a database...\n");
	res_db = sqlite3_open("modbus",&db);
	if(res_db != SQLITE_OK){
		fprintf(stderr, "打开数据库失败: %s\n", sqlite3_errmsg(db));
		return 0;
	}
	
	/* 建数据库表：modbusorder */
	printf("main_Function:Begin to create table:modbusorder...\n");
	sqlcreate(sql,"modbusorder","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusorder error:%s\n", errMsg);
	}
	
	/* 建数据库表：modbusdata */
	printf("main_Function:Begin to create table:modbusdata...\n");
	sqlcreate(sql,"modbusdata","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusdata error:%s\n", errMsg);
	}
	printf("main_Function:create database and table successfully.\n\n");

	/**----------------------------------------------------*/
	
	/* 先标识未连接上服务器 */
	isConnectServer=FALSE; 
	
	/* 创建一个线程，用来接收server服务器程序发送过来的消息 */
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

	/*  这个死循环的作用是保持主线程不关闭，这样子线程就能一直运行 */
	while(1){
		sleep(10000);
	}

}

/*---处理server没有上线时的情况

1.建立一个小线程，不停地询问server程序是否上线

2.当监测到server程序上线后，停止询问

3.当出现server程序下线的情况时，重启这个小线程，重新开始询问server程序是否上线 

4.-难点：c语言并不会抛出异常，如何监测到server程序下线的异常？

*/
void *thread_connectServer(void *arg){
	
	/* 连接server服务器程序 */
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
	
	/* 建立循环，等待server程序上线 */
	while(1){
		printf("\n"); // 用一个换行，与上一个循环过程间隔开来
		if(connect(sock_fd,(struct sockaddr *)&addr_serv,sizeof(struct sockaddr))<0){
			//perror("connectServer_Thread: connect server failed\n");
			//printf("connectServer_Thread: connect (%d) , continue to connect to server...\n",errno);			
			//exit(1);
			printf("connectServer_Thread: connect server failed , continue to connect to server...\n");
		}else{
			printf("connectServer_Thread: connect server successfully\n\n");
			isConnectServer=TRUE; // 将连接server标识量设为true
			break;
		}
		sleep(SLEEPTIME);
	}
	
	/* 创建一个线程，用来接收server服务器程序发送过来的消息 */
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
	
	sleep(SLEEPTIME); // 暂停一下，等待接受线程启动成功
	
	/* 发送自身类型给server，表明自己的身份 */
	int send_num; // 记录发送的字节数
	unsigned char send_buff[64]; // 发送缓冲区
	int recv_num; // 记录接收的字节数
	unsigned char recv_buff[64]; // 接收缓冲区
	
	printf("connectServer_Thread: begin to send type [identity myself: 0x01]......\n");
	send_buff[0]=0x01; // 自身身份是01
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	
	/* 开始准备打开串口 */
	printf("connectServer_Thread: begin to open COM......\n");
	//设置串口属性 
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
		
		// 开始设置串口属性
		tcflush(com_fd,TCIOFLUSH);
		if(tcsetattr(com_fd,TCSANOW,&newtio) != 0){
			printf("connectServer_Thread: set COM2 attribute failed\n");
			isopen = FALSE;
			//exit(1);
		}else{
			printf("connectServer_Thread: set COM2 attribute successfully\n\n");
		}	
	}

	//根据标识量决定发送什么字符给server
	if(isopen == TRUE){
		send_buff[0]=0x04; // 打开串口及设置参数成功
		isOpenCom=TRUE;
		printf("connectServer_Thread: open COM state successfully\n\n");
	}else{
		send_buff[0]=0x05; // 打开串口及设置参数失败
		isOpenCom=FALSE;
		printf("connectServer_Thread: open COM state failed\n\n");
	}
	
	sleep(SLEEPTIME);
	
	/* 根据isopen 的值判断是否打开成功，并将打开与否告诉server */
	printf("connectServer_Thread: begin send type [open COM state]......\n");
	
	send_num=send(sock_fd,send_buff,1,0); //-------------------send type
	if(send_num < 0){
		perror("connectServer_Thread: send type failed\n");
		exit(1);
	}else{
		printf("connectServer_Thread: send type successfully:%x\n\n",send_buff[0]);
	}
	
	
	/* 创建一个线程，处理modbus终端与下位设备之间的通信，以及处理监测数据 */
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


/*---处理终端与server服务器程序之间的消息通信

1.因为这边接收数据也是采用多线程，可能会出现多次接收的情况

2.接收到消息后，不一定需要再次将消息返回，防止无线传输的情况出现

3.程序一开始就建立“接收数据“的线程，这样接收的消息统统有接收进程进行处理

4.程序的其他地方一律不再处理接收消息的代码

5.消息发送的规则是，消息源是谁，消息就到哪儿终止，防止出现消息往复的情况*********

*/
void *thread_recv(void *arg){
	int send_num;
	unsigned char send_buff[64];
	int recv_num;
	unsigned char recv_buff[64];
	
	unsigned char modbusorder[8]; // 发送给下位设备的modbus命令
	unsigned char recv_device[64]; // 接收下位设备返回的modbus消息
	
	int isModbusoederExist ;
	
	while(isConnectServer){
		printf("\n"); // 用一个换行，与上一个循环过程间隔开来
		printf("recv_Thread: begin to recv Msg from server...\n");
		memset(recv_buff,0,sizeof(recv_buff));
		recv_num=recv(sock_fd,recv_buff,sizeof(recv_buff),0); //-------------------recv
		if(recv_num < 0){
			perror("recv_Thread: recv Msg failed\n");
			exit(1);
		}else{
			printf("recv_Thread: recv Msg successfully\n");
		}
		
		// 判断接收的数据类型,第一个字节，根据不同的类型进行不同的处理
		switch(recv_buff[0]){
			case 0x01: // 发送自身类型给server，表明自己的身份：不需要返回，防止消息无限往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x01\n");
				break;
				
			case 0x04: // 串口打开成功消息；不需要返回，防止消息无线往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x04\n");
				break;
				
			case 0x05: // 串口打开失败消息；不需要返回，防止消息无线往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x05\n");
				break;
				
			case 0x07: // server发送modbus命令给modbus终端消息；需要返回，因为这条消息的源是“server服务器程序”
				printf("recv_Thread: the type is 0x07\n");
				
				// 组装返回的消息，说明已经收到该命令
				send_buff[0] = 0x07;
				send_buff[1] = recv_buff[1];
				send_num=send(sock_fd,recv_buff,2,0); //-------------------send
				
				// 将modbus命令放入缓冲区
				int i,result;
				for(i=0;i< MODBUSORDERLENGTH ;i++){ // 获取modbus命令
					modbusorder[i]=recv_buff[i+1];
					//printf("%x ",modbusorder[i]);
				}
				//printf("\n");
				// 加锁-数据库
				pthread_mutex_lock(&mutex_db);
				
				//首先判断数据库中是否已经存在这条modbus命令
				sqlite3_stmt * stat;
				sqlite3_prepare( db, "select * from modbusorder where data = ?;", -1, &stat, 0 );
				sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdata为数据缓冲区，length_of_data_in_bytes为数据大小，以字节为单位
				if((result = sqlite3_step( stat )) != SQLITE_ROW){ // 说明没找到该行数据
					sqlite3_finalize( stat ); //把刚才分配的内容析构掉
					
					// 插入新的数据
					sqlite3_prepare( db, "insert into modbusorder values(?);", -1, &stat, 0 );
					sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdata为数据缓冲区，length_of_data_in_bytes为数据大小，以字节为单位
					result = sqlite3_step( stat );
					sqlite3_finalize( stat ); //把刚才分配的内容析构掉
				}
				
				// 解锁-数据库
				pthread_mutex_unlock(&mutex_db);
				break;
				
			default:
				printf("recv_Thread: %x,this type can not be recognized!\n",recv_buff[0]);
				break;
		}
	}
}




/*---线程：从数据库中读取modbus命令，并将modbus命令送给server或者缓存到本地

1.设置标识量，标志是否已经连接数据库

2.如果server不在线，则直接缓存modbusdata在数据库中

3.如果server在线，则直接将数据返回给server，并开始查询数据库，将查出来的数据，统统返回给server,返回一条删除一条

*/
void *thread_modbusdata(void *arg){
	int send_num;
	unsigned char send_buff[64];
	int recv_num;
	unsigned char recv_buff[64];
	//unsigned char send_device[8]; // 发送给下位设备的modbus命令
	unsigned char recv_device[64]; // 接收下位设备返回的modbus消息
	//char *recv_d = send_device;
	
	
	printf("modbusdata_Thread: the thread is beginning...\n");
	while(1){
		
		/* 首先从modbusorder中读取modbus命令 */
		// 加锁-数据库
		pthread_mutex_lock(&mutex_db);
		
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusorder;", -1, &stat, 0 );
		int result,j ;
		
		while((result = sqlite3_step( stat )) == SQLITE_ROW){
			unsigned char *recv_d = (unsigned char *)sqlite3_column_blob( stat, 0 ); // 指针赋值的时候，该指针指向了另外一片区域
			int len = sqlite3_column_bytes( stat, 0 );
			printf("modbusdata_Thread: the modbusorder is:%d,%s\n",len,recv_d);
			/*
			for(j = 0;j < len;j++){
				printf("%x ",recv_d[j]);
			}
			printf("\n");
			*/
			// 将modbus消息发送给下位设备
			if(write(com_fd,recv_d,len) < 0 ){
				printf("modbusdata_Thread: send modbus order to device failed\n");
			}else{
				printf("modbusdata_Thread: send modbus order to device successfully\n");
			}
					
			// 从下位设备接收数据
			printf("modbusdata_Thread: begin to recive modbus data from device...\n");
			int num_read;
			memset(recv_device,0,sizeof(recv_device));
					
			// 判断是否从下位设备接收到数据，通过是否能够接收到数据判定设备是否在线
			if((num_read = read(com_fd,recv_device,64)) > 0){
				printf("modbusdata_Thread: recv modbus data from device successfully , Len:%d\n",num_read);

			}else{
				printf("modbusdata_Thread: recv modbus data from device failed, Len:%d\n",num_read);
				printf("modbusdata_Thread: recv modbus data from device failed\n");						
			}
			
			
		}
		printf("\n");
		sqlite3_finalize( stat ); //把刚才分配的内容析构掉
		
		// 解锁-数据库
		pthread_mutex_unlock(&mutex_db);
		
		sleep(SLEEPTIME);
	}
}










/**----------------------------------------------------------------------------------------------*/

/*---处理modbus设备仪表的上下线问题，安排在处理0x07类型的消息处进行

1.从modbus命令中提取出设备号；

2.如果从下位设备没有接收到数据，则说明下位设备不在线；

3.如果从下位设备接收到数据，则说明下位设备在线；

4.最后将设备上线、下线的消息发送给server服务器端；

*/


/*---处理modbus消息缓存的问题

1.终端从server接收到modbus命令

2.终端将modbus命令进行缓存

3.缓存方式：采用文件的方式缓存

4.文件操作：[modbus命令文件]
	1.设置文件读写标识量，因为modbus命令文件可能会更新
	2.每次更新的时候需要先将文件清空
	3.文件读取的时候，按顺序读取就行

5.终端从modbus命令文件中按顺序读取数据，然后发送给下位设备

6.终端接收从下位设备返回的数据

7.终端判断server是否在线
	1.如果在线，则将数据直接返回给server
	2.如果不在线，则将数据缓存，缓存方式：文件
	
8.文件操作：[modbus数据]
	1.设置文件读写标识量，不能同时读写
	
	date -s 031413312016 //设置时间(月日时分年)
*/

/**----------------------------------------------------------------------------------------------*/









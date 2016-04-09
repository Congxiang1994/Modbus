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


/* -标识量定义区 */ 
int isOpenCom; // 标识是否打开串口
int isConnectServer; // 标识是否连接server程序
int isGetModbusData; // 标识thread_modbusdata线程是否正常运行
int isSendModbusdataToServer; // 标识是否开启“将数据库中的modbusdata发给server服务器程序”的线程
int isGetModbusDataCanSendModbusdataToServer; // 标识thread_modbusdata线程是否能够开始发送数据给server，因为thread_connectServer需要第一次发送数据

/* -线程同步：数据库读写 */
pthread_mutex_t mutex_db; // 数据库互斥信号量，一次只有一个线程能够访问数据库

/* -函数声明区 */
void *thread_recv(void *arg); // 处理终端与server服务器程序之间的消息通信
void *thread_connectServer(void *arg); // 处理询问server程序是否上线
void *thread_modbusdata(void *arg); // 处理modbus终端与下位设备之间的通信，以及处理监测数据
void *thread_keyboard(void *arg); // 处理键盘事件，设置快捷键终止程序，这样就能正常的关闭程序了
void *thread_sendModbusdataToServer(void *arg); // 将数据库中的数据返回给server服务器程序

int sendMsgToServer(volatile int sock_fd, unsigned char typeOfMsg, unsigned char *contentOfMsg, int lengthOfMsg);

void openCOM(); // 打开串口函数

int sqlcreate(char *sql, char *table, char *key, char *kind);
int sqlinsert(char *sql, char *table, char *key, char *value);
int sqldelete(char *sql, char *table, char *key, char *value);
int sqlselect(char *sql, char *table, char *key, char *value);
int select_callback(void *data, int n_columns, char **column_values, char **column_names);

/**---ctrl+c的消息响应事件
1.结束正在运行的线程

2.正常退出程序

*/
void Stop(int signo) 
{
	printf("Stop: Begin to end this program\n");

	isGetModbusData = FALSE; // 结束thread_modbusdata线程
	isConnectServer = FALSE;
	isGetModbusDataCanSendModbusdataToServer = FALSE;
	
	// 加锁-数据库
	////pthread_mutex_lock(&mutex_db);
	sqlite3_close(db);
	db=0;
	close(sock_fd);
	sleep(SLEEPTIME);
	printf("Stop: EXIT!!!\n");
	exit(0);
	
	// 解锁-数据库
	////pthread_mutex_unlock(&mutex_db);
}

/**---主函数

1.建立数据库及表

2.设置串口参数，并打开串口

3.创建线程：从下位设备采集数据

4.创建线程：请求与server服务器的连接

*/
int main(){
	/* 先标识未连接上服务器 */
	isConnectServer=FALSE; 
	
	/* 置采集监测数据线程的循环变量为TRUE */
	isGetModbusData = TRUE;
	
	/* 置采集监测数据线程的“能否发送数据给server”的标识量为FALSE */
	isGetModbusDataCanSendModbusdataToServer = FALSE;
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* 初始化数据库互斥信号量 */
	int init = pthread_mutex_init(&mutex_db, NULL);
	if(init != 0)
	{
	   printf("main_Function: mutex_db init failed \n");
	   return 0;
	} 
	/**--------------------------------------------------------------------------------------------------------*/
	
	/* 建立数据库及数据库表 */
	// 1.建立数据库 
	printf("main_Function: Begin to create a database...\n");
	res_db = sqlite3_open("modbus",&db);
	if(res_db != SQLITE_OK){
		fprintf(stderr, "打开数据库失败: %s\n", sqlite3_errmsg(db));
		return 0;
	}
	
	// 2.建数据库表：modbusorder 
	printf("main_Function:Begin to create table:modbusorder...\n");
	sqlcreate(sql,"modbusorder","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusorder error:%s\n", errMsg);
	}
	
	// 3.建数据库表：modbusdata
	printf("main_Function:Begin to create table:modbusdata...\n");
	sqlcreate(sql,"modbusdata","data","VARCHAR(128)");
	res_db = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
	if(res_db != SQLITE_OK){
		printf("main_Function:create table modbusdata error:%s\n", errMsg);
	}
	printf("main_Function:create database and table successfully.\n\n");
	sqlite3_close( db ); // 关闭数据库
	db=0;
	signal(SIGINT, Stop); 
	/**--------------------------------------------------------------------------------------------------------*/
	
	
	/* 打开串口 */
	openCOM();
	/**--------------------------------------------------------------------------------------------------------*/
	
	
	/* 创建一个线程，处理modbus终端与下位设备之间的通信，以及处理监测数据 */
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
	
	/* 创建一个线程，用来请求连接server服务器程序 */
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
	
	/* 这个死循环的作用是保持主线程不关闭，这样子线程就能一直运行 */
	while(1){
		sleep(10000);
	}

}

/**---线程：请求连接server服务器

1.不停的询问server服务器是否在线

2.告诉server服务器程序，自己modbus终端的身份

3.告诉server服务器程序，串口是否打开成功

4.创建线程：从下位设备采集数据

*/
void *thread_connectServer(void *arg){
	
	/* 连接server服务器程序 */
	// 1.创建套接字
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
	
	// 2.建立循环，等待server程序上线
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
	/**--------------------------------------------------------------------------------------------------------*/
	
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
	/**--------------------------------------------------------------------------------------------------------*/
	
	sleep(SLEEPTIME); // 暂停一下，等待接受线程启动成功
	
	/* 发送自身类型给server，表明自己的身份 */
	unsigned char typeOfMsg ; // 消息类型
	int send_num; // 记录发送的字节数
	unsigned char send_buff[64]; // 发送缓冲区
	int recv_num; // 记录接收的字节数
	unsigned char recv_buff[64]; // 接收缓冲区
	
	printf("connectServer_Thread: begin to send type [identity myself: 0x01]......\n");
	/*
	send_buff[0]=0x01; // 自身身份是01
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

	sleep(SLEEPTIME); // 暂停一下
	/* 将串口是否成功打开的消息告诉server服务器程序 */
	if(isOpenCom == TRUE){
		typeOfMsg=0x04; // 打开串口及设置参数成功
	}else{
		typeOfMsg=0x05; // 打开串口及设置参数失败
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
	
	sleep(SLEEPTIME); // 暂停一下
	isGetModbusDataCanSendModbusdataToServer = TRUE;
	/**--------------------------------------------------------------------------------------------------------*/
	

	/* 创建一个将数据库中的监测数据返回给server的线程 */
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


/**---处理终端与server服务器程序之间的消息通信

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
	
	unsigned char typeOfMsg ; // 发送的消息类型
	
	unsigned char modbusorder[8]; // 发送给下位设备的modbus命令
	unsigned char recv_device[64]; // 接收下位设备返回的modbus消息
	
	char currentTime[64]; // 用来存储server发送过来的系统时间
	
	int isModbusoederExist ;
	
	while(isConnectServer){
		printf("\n"); // 用一个换行，与上一个循环过程间隔开来
		printf("recv_Thread: begin to recv Msg from server...\n");
		memset(recv_buff,0,sizeof(recv_buff));
		
		/*
		recv_num=recv(sock_fd,recv_buff,sizeof(recv_buff),0); //-------------------recv
		*/
		recv_num = recvMsgFromServer(sock_fd, recv_buff, sizeof(recv_buff)); // 调用自己重写的recv函数
		
		if(recv_num < 0){
			printf("recv_Thread: recv Msg failed\n");
			isConnectServer = FALSE;
			isGetModbusDataCanSendModbusdataToServer = FALSE;
			/* 这里需要重启请求连接server服务器程序的线程！！！！！！ */
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
			
			return 0; // 这个线程可以返回结束了
			//exit(1);
		}else{
			printf("recv_Thread: recv Msg successfully\n");
		}
		
		// 判断接收的数据类型,第一个字节，根据不同的类型进行不同的处理
		switch(recv_buff[0]){
			case 0x01: // 发送自身类型给server，表明自己的身份：不需要返回，防止消息无限往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x01\n");
				break;
				
			case 0x02: // 发送下位设备上线的消息类型给server：不需要返回，防止消息无限往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x02\n");
				break;	
	
			case 0x03: // 发送下位设备下线的消息类型给server：不需要返回，防止消息无限往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x03\n");
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
				
				// 将modbus命令放入缓冲区
				int i,result;
				for(i=0;i< MODBUSORDERLENGTH ;i++){ // 获取modbus命令
					modbusorder[i]=recv_buff[i+1];
					//printf("%x ",modbusorder[i]);
				}
				//printf("\n");
				// 加锁-数据库
				pthread_mutex_lock(&mutex_db);
				res_db = sqlite3_open("modbus",&db);
				// 1.首先判断数据库中是否已经存在这条modbus命令
				sqlite3_stmt * stat;
				sqlite3_prepare( db, "select * from modbusorder where data = ?;", -1, &stat, 0 );
				sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdata为数据缓冲区，length_of_data_in_bytes为数据大小，以字节为单位
				result = sqlite3_step( stat );
				sqlite3_finalize( stat ); // 把刚才分配的内容析构掉
				if(result != SQLITE_ROW){ // 说明没找到该行数据
					// 2.插入新的数据
					sqlite3_prepare( db, "insert into modbusorder values(?);", -1, &stat, 0 );
					sqlite3_bind_blob( stat, 1, modbusorder, MODBUSORDERLENGTH , NULL ); // pdata为数据缓冲区，length_of_data_in_bytes为数据大小，以字节为单位
					result = sqlite3_step( stat );
					printf("recv_Thread: the result of sqlite3_step is:%d\n",result);
					sqlite3_finalize( stat ); //把刚才分配的内容析构掉
				}
				sqlite3_close(db);
				db=0;
				// 解锁-数据库
				pthread_mutex_unlock(&mutex_db);
				break;
				
			case 0x09: // server发送当前系统时间给modbus终端消息；需要返回，因为这条消息的源是“server服务器程序”
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
				
				// 1.获取从server发送过来的时间
				printf("recv_Thread: Begin to get the current time\n");
				int k;
				for(k = 1; k< recv_num; k++){
					currentTime[k-1] = recv_buff[k];
				}
				printf("recv_Thread: the currentTime is:%s\n",currentTime);
				
				// 2.更新系统时间
				printf("recv_Thread: Begin to update system time\n");
				if(SetSystemTime(currentTime) == 0){
					printf("recv_Thread: Update system time successfully\n");
				}else{
					printf("recv_Thread: Update system time failed\n");					
				}
				break;
				
			case 0x0A: // modbus终端返回监测数据给server服务器消息；不需要返回，防止消息无线往复，因为这条消息的源是“modbus终端”
				printf("recv_Thread: the type is 0x0A\n");
				break;
				
			default:
				printf("recv_Thread: %x,this type can not be recognized!\n",recv_buff[0]);
				//exit(1); // 出现错误，先退出
				break;
		}
	}
}




/**---线程：从下位设备采集modbusdata监测数据

1.从数据库中获取modbusorder命令

2.如果“modbusdata不为空”或者“未连接server服务器程序”，则将modbusdata缓存到本地数据库中

3.如果“modbusdata为空”并且“已连接server服务器程序”，则将modbusdata直接发送给server服务器

*/
void *thread_modbusdata(void *arg){
	int send_num; // 发送给server服务器程序的字节数量
	unsigned char recv_device[64]; // 接收下位设备返回的modbus消息
	unsigned char modbus_buff[128]; // 数据缓冲：当前系统时间+modbus数据
	unsigned char typeOfMsg ; // 发送的消息类型
	
	unsigned char isDeviceOnOrOff; // 标识下位设备是上线还是下线
	
	printf("modbusdata_Thread: the thread is beginning...\n");
	while(isGetModbusData){
		
		/* 从modbusorder中读取modbus命令 */
		// 加锁-数据库
		pthread_mutex_lock(&mutex_db);
		res_db = sqlite3_open("modbus",&db);
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusorder;", -1, &stat, 0 );
		int result,j,k,s,len; // len:modbus命令的长度相同
		unsigned char recv_d[32][8]={0}; // 假设最多32条命令
		k = 0;
		while((result = sqlite3_step( stat )) == SQLITE_ROW){
			memcpy(recv_d[k],sqlite3_column_blob( stat, 0 ),8);
			//recv_d[k] = (unsigned char *)sqlite3_column_blob( stat, 0 ); // 指针赋值的时候，该指针指向了另外一片区域
			len = sqlite3_column_bytes( stat, 0 );
			printf("modbusdata_Thread: !!!!!!the No.%d modbusorder is:%d,%s\n",k,len,recv_d[k]);
			k++;
		}
		sqlite3_finalize( stat ); //把刚才分配的内容析构掉
		printf("modbusdata_Thread: the num of modbusorder is:%d\n",k);
		sqlite3_close(db);
		db=0;
		// 解锁-数据库
		pthread_mutex_unlock(&mutex_db);

		for(s = 0;s < k; s++){
			printf("modbusdata_Thread: No.%d modbus order is:%s\n",s,recv_d[s]);
		}
		
		for(s = 0;s < k; s++){
			//////////////////sqlite3_finalize( stat ); //把刚才分配的内容析构掉
			printf("modbusdata_Thread: Begin to send No.%d modbus order to device:%s\n",s,recv_d[s]);
			// 将modbus消息发送给下位设备
			if(write(com_fd,recv_d[s],len) < 0 ){
				printf("modbusdata_Thread: send modbus order to device failed\n");
			}else{
				printf("modbusdata_Thread: send modbus order to device successfully\n");
			}
					
			// 从下位设备接收数据
			printf("modbusdata_Thread: begin to recive modbus data from device...\n");
			int num_read;
			memset(recv_device,0,sizeof(recv_device));
					
			/* 将下位设备状态的标识量初始化，置为False */
			isDeviceOnOrOff = 0x03;
			
			// 判断是否从下位设备接收到数据，通过是否能够接收到数据判定设备是否在线
			if((num_read = read(com_fd,recv_device,64)) > 0){
				
				/* 标识该下位设备是上线还是下线 */
				isDeviceOnOrOff = 0x02; // 下位设备上线，标识量置为TRUE
				
				int i;
				printf("modbusdata_Thread: recv modbus data from device successfully , Len:%d\n",num_read);
				for(i = 0;i < num_read ;i++ ){
					printf("%x ",recv_device[i]);
				}
				printf("\n");
				
				/*组装modbusdata数据：时间 + data*/
				unsigned char str_time[19];
				getSystemTime(str_time);
				printf("modbusdata_Thread: the currentTime is:%s\n",str_time);
				
				memset(modbus_buff,0,sizeof(modbus_buff));
				strcat(modbus_buff,str_time); // 将时间添加到modbus_buff中
				
				// 注意：modbus数据是16进制数，不能直接按照处理字符串的方式直接将字符串进行连接,需要通过一个循环来接收数据
				for(i = 0; i < num_read; i++){
					modbus_buff[19+i] = recv_device[i];
				}
				
				printf("modbusdata_Thread: the modbusdata is:");
				for(i = 19;i < num_read + 19;i++ ){
					printf("%x ",modbus_buff[i]);
				}
				printf("\n");
				// ------至此，组装“时间 + data”成功，modbus_buff的长度为：19+num_read
				
				
				/* 根据实际情况，对modbus终端采集的数据进行处理 */
				// 1.如果“modbusdata不为空”或者“未连接server服务器程序”，则将modbusdata缓存到本地数据库中
				// 2.如果“modbusdata为空”并且“已连接server服务器程序”，则将modbusdata直接发送给server服务器
				// 加锁-数据库
				pthread_mutex_lock(&mutex_db);
				res_db = sqlite3_open("modbus",&db);
				sqlite3_stmt * stat_data;
				sqlite3_prepare( db, "select * from modbusdata;", -1, &stat_data, 0 );
				int result_data = sqlite3_step( stat_data );
				sqlite3_finalize( stat_data ); //把刚才分配的内容析构掉
				// 解锁-数据库
				pthread_mutex_unlock(&mutex_db);
							
				if( result_data == SQLITE_ROW || isGetModbusDataCanSendModbusdataToServer == FALSE){ // 数据库中存在监测数据 || 未连接server服务器程序，则将监测数据插入数据库modbusdata中
					
					printf("modbusdata_Thread: the table modbusdata is not empty or has not connected the server .\n");
					
					
					// 将数据直接放进数据库中,
					//result = sqlite3_exec( db, "begin transaction", 0, 0, &errMsg ); // 开始一个事务					
					// 加锁-数据库
					pthread_mutex_lock(&mutex_db);
					sqlite3_prepare( db, "insert into modbusdata values(?);", -1, &stat_data, 0 );
					sqlite3_bind_blob( stat_data, 1, modbus_buff, num_read + 19 , NULL ); // pdata为数据缓冲区，length_of_data_in_bytes为数据大小，以字节为单位
					result = sqlite3_step( stat_data );
					printf("modbusdata_Thread: the result of sqlite3_step is:%d\n",result);
					sqlite3_finalize( stat_data ); //把刚才分配的内容析构掉
					//result = sqlite3_exec( db, "commit transaction", 0, 0, &errMsg ); // 提交事务
					sqlite3_close(db);
					db=0;
					// 解锁-数据库
					pthread_mutex_unlock(&mutex_db);

				}else{ // 数据库中不存在监测数据 && 已经连接server服务器程序，则将监测数据直接返回给server服务器程序
					printf("modbusdata_Thread: the table modbusdata is empty and has connected the server.\n");
/*
					// 将modbus消息整体往后挪一位，在第一位加上0x0A
					for(i = num_read+19; i > 0; i--){ 
						modbus_buff[i+1]=modbus_buff[i];
					}
					modbus_buff[0] = 0x0A;
					
					// 将消息返回给server服务器程序
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
			
			/* 将下位设备是否上线的消息发送给上位机 */
			// 1.判断是否能够向server发送数据
			if(isGetModbusDataCanSendModbusdataToServer == TRUE){
				if(sendMsgToServer(sock_fd, isDeviceOnOrOff, recv_d[s], 1)){ // 只需要将下位设备ID发送过去就行
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

/**---线程：将数据库中的监测数据返回给server程序

1.前提：已连接server服务器程序

2.每次从数据库中取一天最久的modbusdata，并将modbusdata发送给server服务器程序

3.成功发送数据后，删除该条modbusdata

*/
void *thread_sendModbusdataToServer(void *arg){
	
	unsigned char modbus_buff[128]; // 数据缓冲：当前系统时间+modbus数据
	
	int send_num; // 发送给server服务器程序的字节数量
	
	unsigned char typeOfMsg ; // 发送的消息类型
	
	printf("sendModbusdataToServer_Thread: the thread is beginning...\n");
	
	while(isConnectServer){
		// 加锁-数据库
		pthread_mutex_lock(&mutex_db);
		
		memset(modbus_buff,0,sizeof(modbus_buff));
		
		res_db = sqlite3_open("modbus",&db);
		sqlite3_stmt * stat;
		sqlite3_prepare( db, "select * from modbusdata;", -1, &stat, 0 );
		int result,len,i; // len:modbusdata的长度
		 //
		result = sqlite3_step( stat );
		//sqlite3_finalize( stat ); //把刚才分配的内容析构掉
		unsigned char recv_d[64]={'\0'};
		if(result == SQLITE_ROW){
			printf("sendModbusdataToServer_Thread: the modbusdata is not empty.\n");
			len = sqlite3_column_bytes( stat, 0 );
			memcpy(recv_d,sqlite3_column_blob( stat, 0 ),len);
			//unsigned char *recv_d = (unsigned char *)sqlite3_column_blob( stat, 0 );
			
			sqlite3_finalize( stat ); //把刚才分配的内容析构掉
			printf("sendModbusdataToServer_Thread: the modbusorder is:%d,%s\n",len,recv_d);
			
			/*
			// 将modbus消息整体往后挪一位，在第一位加上0x0A
			for(i = len; i > 0; i--){ 
				modbus_buff[i+1]=recv_d[i];
			}
			modbus_buff[0] = 0x0A;	
			// 将消息返回给server服务器程序
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
			// 将该条modbusdata删除
			printf("sendModbusdataToServer_Thread: begin to delete the modbusdata\n");
			sqlite3_prepare( db, "delete from modbusdata where data = ? ;", -1, &stat, 0 );
			sqlite3_bind_blob( stat, 1, recv_d, len , NULL ); 
			result = sqlite3_step( stat );
			printf("sendModbusdataToServer_Thread: the result of sqlite3_step is:%d\n",result);
			sqlite3_finalize( stat ); //把刚才分配的内容析构掉
			printf("sendModbusdataToServer_Thread: delete the modbus data:%s\n",recv_d);
		}else{
			sqlite3_finalize( stat ); //把刚才分配的内容析构掉
		}
		
		// 解锁-数据库
		pthread_mutex_unlock(&mutex_db);
		
		sqlite3_close(db);
		db=0;
		
		sleep(1);		
	}
}
/************************************************ 
函数：设置modbus终端上的串口参数，打开串口
**************************************************/  
void openCOM(){
	/* 开始准备打开串口 */
	printf("openCOM: begin to open COM......\n");
	// 1.设置串口属性 
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
		
		// 开始设置串口属性
		tcflush(com_fd,TCIOFLUSH);
		if(tcsetattr(com_fd,TCSANOW,&newtio) != 0){
			printf("openCOM: set COM2 attribute failed\n");
			isopen = FALSE;
			//exit(1);
		}else{
			printf("openCOM: set COM2 attribute successfully\n\n");
		}	
	}

	// 2.根据标识量决定发送什么字符给server
	if(isopen == TRUE){
		isOpenCom = TRUE;
		printf("openCOM: open COM state successfully\n\n");
	}else{
		isOpenCom = FALSE;
		printf("openCOM: open COM state failed\n\n");
	}
}

/************************************************ 
设置操作系统时间 
参数:*dt数据格式为"2006-4-20 20:30:30" 
调用方法: 
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
获取操作系统时间 
格式："2006-04-20 20:30:30"; 长度要一致 长度为19
sprintf(char *string, char *format,arg1,arg2...);
%02d:不足的话补‘0’
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
/**---函数：发送消息给server服务器程序
参数：
1.volatile int sock_fd; // 套接字的文件描述符
2.char *typeOfMsg; // 消息类型
3.char *contentOfMsg; // 消息内容
4.int lengthOfMsg; // 消息内容的长度(不包括消息类型)
返回值：
1:成功发送数据
0:发送数据失败
*/
int sendMsgToServer(volatile int sock_fd, unsigned char typeOfMsg, unsigned char *contentOfMsg, int lengthOfMsg){
/*	
	// 1.待发送数据的缓冲区，并初始化为空
	int send_num; // 记录发送的字节数
	unsigned char send_buff[128]; 
	memset(send_buff,0,sizeof(send_buff)); // 初始化为空
	
	// 2.添加消息类型
	send_buff[0]=typeOfMsg; // 消息类型就一个字节
	printf("sendMsgToServer: type:0x%02x\n",send_buff[0]);
	
	// 3.添加消息内容
	int i;
	for(i = 0;i < lengthOfMsg; i++){
		send_buff[i+1] = contentOfMsg[i];
	}
	printf("sendMsgToServer:Begin to  send Msg\n");
	
	// 4.发送数据
	send_num = send(sock_fd,send_buff,lengthOfMsg+1,0); //-------------------send type
	if(send_num < 0){
		printf("sendMsgToServer: send type failed:%x\n\n",send_buff[0]);
		return 0; // 发送失败
	}else{
		printf("sendMsgToServer: send type successfully:%x\n\n",send_buff[0]);
		return 1; // 发送成功
	}
*/	
	/***-------------------------------------------------------------------------------*/
	/* 重写发送数据的函数 */
	// 1.定义发送缓冲区，缓冲区的长度为：lengthOfMsg + 1 + 1 + 1（这里的lengthOfMsg不包括消息类型码）
	int send_num; // 记录发送的字节数
	int i;
	unsigned char send_buff[128];
	memset(send_buff,0,sizeof(send_buff)); // 初始化为空
	
	// 2.在数据缓冲区中添加“消息体长度”字段：1个字节，用16进制数表示
	send_buff[0] = (char)(lengthOfMsg + 1); // 这里的lengthOfMsg是不包括消息类型的
	
	// 3.在数据缓冲区中添加“消息类型码 + 消息体”：lengthOfMsg + 1个字节
	send_buff[1] = typeOfMsg;
	printf("sendMsgToServer: type:0x%02x\n",send_buff[1]);
	for(i = 0;i < lengthOfMsg; i++){
		send_buff[i+2] = contentOfMsg[i];
	}

	// 4.在数据缓冲区中添加“停止符号”：1个字节
	send_buff[lengthOfMsg+2] = 0xFF;
	
	// 5.发送缓冲区中所有数据：消息长度 + 消息类型码 + 消息体 + 停止符号0xFF
	printf("sendMsgToServer:Begin to send Msg\n");
	send_num = send(sock_fd,send_buff,lengthOfMsg + 3,0); //-------------------send type
	if(send_num < 0){
		printf("sendMsgToServer: send type failed:%x\n\n",send_buff[1]);
		return 0; // 发送失败
	}else{
		printf("sendMsgToServer: send type successfully:%x\n\n",send_buff[1]);
		return 1; // 发送成功
	}
}
/**---函数：接收从server服务器发送的消息(替代recv的函数，不是线程)
参数：
1.volatile int sock_fd; // 套接字的文件描述符
2.unsigned char *recv_buff; // 接收缓冲区
3.int lengthOfRecvBuff; // 接收缓冲区的大小
返回值：
recv_num
*/
int recvMsgFromServer(volatile int sock_fd, unsigned char *recv_buff, int lengthOfRecvBuff){
/*	
	int recv_num; // 接收的字节数
	memset(recv_buff,0,strlen(recv_buff)); // 将缓冲区清空
	
	recv_num=recv(sock_fd, recv_buff, lengthOfRecvBuff, 0); //-------------------recv
	return recv_num;
*/
	/***-------------------------------------------------------------------------------*/
	/* 重写接收数据的函数 */
	unsigned char byteTemp[1];
	int recv_num;
	// 1.读入一个字符：消息体的长度lengthOfMsg
	memset(byteTemp,0,strlen(byteTemp)); // 将缓冲区清空
	recv(sock_fd, byteTemp, 1, 0); //-------------------recv
	int lengthOfMsg = (int) byteTemp[0];
	
	// 2.读入lengthOfMsg个字符
	recv_num = recv(sock_fd, recv_buff, lengthOfMsg, 0); //-------------------recv
	
	// 3.再读入一个字符，判断是不是“停止符号0xFF”
	memset(byteTemp,0,strlen(byteTemp)); // 将缓冲区清空
	recv(sock_fd, byteTemp, 1, 0); //-------------------recv
	
	if(byteTemp[0] == 0xFF){
		// 4.如果是停止符号，并且recvNum == lengthOfMsg 则说明接收正确
		return recv_num;
	}else{
		// 5.如果不是停止符号，或者recvNum != lengthOfMsg，则说明接收错误。>>>接收错误处理:不停的读入下一个字符，知道读到0xFF为止
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
/*
查询16进制的数
select hex(data) from modbusorder;
1 3 14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
1 3 14 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0

01 03 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 a3 67 00
*/
/*---存在的问题：
1.当ctrl+c终止程序后，再次启动程序时，sqlite3会生成一个-journal的文件，导致无法正常插入数据

2.sqlite3_finalize( stat_data ); //把刚才分配的内容析构掉！！！！！！！！！！绝对要配对，不能缺少，不然会导致错误
*/

/*---存在的问题：
1.thread_modbusdata会提前一步发送功能码给server服务器，导致server服务器不能正确识别是什么设备在请求连接

*/






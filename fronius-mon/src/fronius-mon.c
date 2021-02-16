/*
 ============================================================================
 Name        : fronius_mon.c
 Author      : Juan Navarro
 Version     :
 Copyright   : Copyright Juan Navarro García. Todos los derechos reservados.
 Descriptio  : Programa de monitorizacion y control
               de inversores Fronius que emplean el
               Fronius Interface Protocol
               descrito en el documento:
               "Fronius InterFace 42,0410,1564   002-04092013"

 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/timerfd.h>
#include <limits.h>
#include <float.h>

#include "registro.h"


#define SIZE_HEADER_FRAME               7 // tamaño de la trama sin el campo de datos y sin checksum
#define SIZE_HEADER_FRAME_PLUS_CHECKSUM 8 // tamaño de la trama sin el campo de datos
#define MAX_SIZE_DATA_FIELD 127 // tamaño maximo del campo de datos (sin checksum)

#define WAIT_TIME_AFTER_REQUEST    100000 // tiempo de espera microsegundo a que llegen datos tras el envio de una peticion
#define RETRIES_TO_RECEIVE_EXPECTED_BYTES 3 //


/* VARIABLES GLOBALES */
char *identificacion = "fronius-mon  Autor:Junavar versión: 69 (16/02/2021)";
char *portname1 = "/dev/ttyUSB0";
//char *portname1 = "/dev/rs422-fronius";

int velocidad_puerto=B19200; //(Macros definidas en termios.h) B1200->0000011; B1800->0000012;B2400->0000013;B4800->0000014;B9600->0000015; B19200->0000016
unsigned char num_inversor=0x01;
char msgerror[1024]; //string para mensaje de error
int potencia_nominal_inversor=4000;
int flag_d=0; // opcion de linea de comando para pintar tramas para depuracion


void configura_puerto_serie(int fd){
  	struct termios tp;
	tcgetattr(fd, &tp); /* se obtiene la actual configuración del puerto */
	cfmakeraw(&tp); /* modo raw */
	tp.c_cflag =  CS8|CREAD|CLOCAL;
	cfsetspeed(&tp, velocidad_puerto); /* velocidad del puerto serie */
    tcsetattr(fd, TCSANOW, &tp);
	tcflush(fd, TCIOFLUSH);
}

struct fronius_frame{
		unsigned char start[3];  // start sequence  - 3 times 0x80
		unsigned char lenght; // number  of bytes in data field
		unsigned char device; // type device, eg  inverter, sensor box, etc
		unsigned char number; // number of relevant device
		unsigned char command;// Query, commnadd to be carried out
		unsigned char data_plus_checksum[MAX_SIZE_DATA_FIELD+1]; 	//variable lenght value of queried command (max. 127 bytes)
												//last byte is the checksum of all byes in frame except
												//start and checksum

} ff_request= {{0x80,0x80,0x80}}, ff_response;

/*
 * Possible Values for the "Device/Option" Byte
0x00 General data query or query to the interface card (the "Number" byte is ignored)
0x01 Inverter
0x02 Sensor card
0x03 Fronius IG datalogger*
0x04 reserved#include <sys/ioctl.h>
0x05 Fronius String Control*
 *
 */


struct data_response_get_version{
	unsigned char type_inverter;
	unsigned char IFC_Major;
	unsigned char IFC_Minor;
	unsigned char IFC_Release;
	unsigned char SW_Major;
	unsigned char SW_Minor;
	unsigned char SW_Release;
	unsigned char SW_Build;
	unsigned char data_checksum;
};


/*
 * Inserta por delante una cadena en otra
 */
int insstr(char * ainsertar, char * cadena)
{
	int desplazar;
	int longitud;
	int i;
	longitud=strlen(cadena);
	desplazar=strlen(ainsertar);
	//printf("%s (%d)-->   %s (%d)\n", ainsertar, desplazar, cadena, longitud);

	cadena[longitud+desplazar]=cadena[longitud]; //se copia el /0 de final de string
	// se mueven los caracteres para dejar sitio a la inserción
	for (i=longitud-1; i>=0; i--){
		cadena[i+desplazar]=cadena[i];
		//printf("\n%s", cadena);
	}
	// se copia los caracteres a insertar en el hueco dejado
	for (i=desplazar-1; i>=0; i--){
			cadena[i]=ainsertar[i];
			//printf("\n%s", cadena);
		}

	//printf("\n");
	return 0;
}

/*
 * Desplaza el bufer de la trama n caracteres hacia la izquierda dejando sitio para más caracteres
 */
void desplazar(char * buffer, int n){

	int i;
	if (n<=0) return;

	for (i=0; i<n; i++){
		buffer[n-1+i]=buffer[n+i];
	}
}

/*
 * Vacia la cola de entrada
 */
int vacia_cola(int fd){
	int bytes_en_cola;
	int rc;
	ioctl(fd, FIONREAD, &bytes_en_cola);
	if (bytes_en_cola>0){
		printf("Bytes en cola de lectura antes de enviar comando: %d\n", bytes_en_cola);
	}
	while(bytes_en_cola>0){
		//se lee sin superar el tamaño del buffer
		rc=read(fd, &ff_response, sizeof(ff_response)<bytes_en_cola?sizeof(ff_response):bytes_en_cola);
		if (rc==0) {
			sprintf(msgerror, "vaciado incompleto de cola de entrada del driver puerto serie");
			return -1;
		}
		bytes_en_cola-=rc;
	}
	return 0;
}

#if 0
void pintartrama(struct fronius_frame *trama){

	int i;
	printf("\n");
	for (i=0; i<3; i++){
		  printf("%d",trama->start[i]);
	}
	printf(" %d",trama->lenght);
	printf(" %d",trama->device);
	printf(" %d",trama->number);
	printf(" %d   ",trama->command);
	for (i=0; i<trama->lenght; i++){
	  printf(" %d",trama->data_plus_checksum[i]);
	}
	printf("     %d", trama->data_plus_checksum[i]);
}
#endif

//#define _pintartramas

/*
 *  Manda un comando  en la trama apuntada por pff_resquest a través del interfaz RS422 a una cadena de inversores
 *  y recibe los resultados de la trama en un buffer apuntado por pff_resquest
 *
 *  Calcula el checksum y lo pone como ultimo byte de la trama
 *
 *  Antes de enviar el comando comprueba que no hay caracteres en la cola de entrada del puerto serie
 *  y en caso contrario los lee para eliminarlos
 *
 *
 */
int static send_command(int fd, struct fronius_frame *pff_request,struct fronius_frame *pff_response)
{

	int rc;
	int i;
	int	bytes_a_sumar;
	int bytes_a_escribir;
	int bytes_a_leer;
	int bytes_en_cola; //byte existentes en la cola de recepción antes de enviar el comando
	int reintentos;
	unsigned char checksum;
	unsigned char *puntero;

	puntero=&pff_request->lenght;

	//el checksum es la suma a 8bits de todos los campos del frame
	//menos los 3 bytes de start y el propio checksum
	checksum=0;
	for(bytes_a_sumar=(pff_request->lenght + 4);bytes_a_sumar>0; bytes_a_sumar--)
	{
		checksum+=*puntero;
		puntero++;
	}
	//se pone el checksum calculado en el último byte que la trama de envío
	pff_request->data_plus_checksum[pff_request->lenght]=checksum;

	// tamaño de los datos + resto de datos
	bytes_a_escribir=SIZE_HEADER_FRAME_PLUS_CHECKSUM + pff_request->lenght;

	if (flag_d){
		unsigned char* tramaw;
		tramaw=(unsigned char *)pff_request;
		for (i=0;i<bytes_a_escribir;i++){
			printf("trama Peticion: TRAMA-W[%d]--> %d\n",i, tramaw[i]);
		}
	}


	//antes enviar un commando se elimina la información de puede haber en la cola de entrada

	rc=vacia_cola(fd);
	if (rc) {
		printf("Error en vaciar_colar:%s\n", msgerror);
	}

	// se manda la orden
	rc= write(fd, pff_request,bytes_a_escribir);
	if (rc!=bytes_a_escribir){
		// No se puede escribir todos los bytes de la petición
		sprintf(msgerror, "Escritura incompleta en dispositivo puerto serie");
		return -1;
	}

	if (flag_d){
	  	printf("Enviados %d de %d\n",rc,bytes_a_escribir);
		printf("Peticion: lenght data        --> %d\n",pff_request->lenght);
		printf("Peticion: device             --> %d\n",pff_request->device);
		printf("Peticion: number             --> %d\n",pff_request->number);
		printf("Peticion: command            --> %d\n",pff_request->command);
		for (i=0; i<pff_request->lenght; i++){
          printf("Peticion: DATA[%d]            --> %d\n",i,pff_request->data_plus_checksum[i]);
		}
		printf("Petición: checksum (DATA[%d]) --> %d\n",i,pff_request->data_plus_checksum[i]);
	}

	// tras esperar hasta que haya, al menos, 7 bytes (justo hasta donde empiezan los datos)
	bytes_a_leer=SIZE_HEADER_FRAME;

	reintentos=0;
	do{
	  usleep(WAIT_TIME_AFTER_REQUEST); // espera un tiempo a que llegen tras la orden
	  ioctl(fd, FIONREAD, &bytes_en_cola);


	  if (flag_d){
		  printf ("Bytes recibidos en cola: %d\n",bytes_en_cola);
	  }

	  reintentos++;
	  /* to_do:
	   * retirar este printf
	   */
	  if (reintentos>1){
		  printf (".");
		  fflush(stdout);
	  }

	  if (reintentos>RETRIES_TO_RECEIVE_EXPECTED_BYTES){
		  sprintf (msgerror,"Too many retries to receive header frame");
		  return -1; // error de lectura
	  }

	}while (bytes_en_cola < bytes_a_leer);


	// se leen los 7 bytes (justo hasta donde empiezan los datos)
	rc= read(fd, pff_response, bytes_a_leer);
	if (rc!=bytes_a_leer){
			sprintf(msgerror, "Lectura incompleta en dispositivo puerto serie");
			return -1;
	}
	// se comprueba el inicio de trama y en caso contrario se alinea leyendo nuevos caracteres;
	int n=0;
	while(n>2){
		if (pff_response->start[n]!=0x80){
			sprintf(msgerror, "Trama erronea: No inicia por 0x80 0x80 0x80");
			return -1;
		}
		else{
			// va bien la cosa comprueba siguiete byte de cabecera
			n++;
		}
	}


	if (flag_d){
		unsigned char* tramar;
		tramar=(unsigned char *)pff_response;
		for (i=0;i<SIZE_HEADER_FRAME;i++){
			printf("trama respuesta: TRAMA-R[%d]--> %d\n",i, tramar[i]);
		}
	}

	if (pff_response->lenght>MAX_SIZE_DATA_FIELD){
		sprintf (msgerror,"Error longitud excesiva de trama recibida");
		return -1;
	}

#if 1  // control de que la trama viene del dispositivo solicitado y responde al comando solicitado
	if ((pff_response->device!=pff_request->device) || (pff_response->number!=pff_request->number)){
	 /*
	  * la trama no viene del dispositivo solicitado
	  *
	  * Ojo que en caso de Broadcast pff_request->number=0 y sin embargo responderá cada uno de los inversores
	  * Posiblemente sea necesario poner un parametro a la función que indique de que inversor se espera la respuesta.
	  */
		sprintf (msgerror,"Trama no procedente del inversor solicitado %d", pff_request->number  );
		return -1; // error de lectura
	}

	if (pff_response->command != pff_request->command){ //la trama de respuesta no corresponde al comando solicitado
		if (pff_response->command==0x0e){ 				//este es un caso especial
			// hay que leer el resto de datos de trama
		}
		else {
			sprintf (msgerror,"La trama de respuesta no corresponde al comando solicitado %d", pff_request->command);
			return -1;
		}
	}
#endif


	// luego se leen tantos como lo que indica el campo del frame pff_response->lenght + un byte para checksum
	bytes_a_leer=(pff_response->lenght)+1; //se pretende leer todos los bytes de datos + el byte de checksum

	ioctl(fd, FIONREAD, &bytes_en_cola);
	reintentos=0;
	while (bytes_en_cola < bytes_a_leer){
		usleep(WAIT_TIME_AFTER_REQUEST ); // espera un tiempo para reintentar
		ioctl(fd, FIONREAD, &bytes_en_cola);
		reintentos++;
		/* to_do:
		 * retirar este printf
		 */
		if (reintentos>1){
			printf (" %d/%d",bytes_en_cola, bytes_a_leer);
			fflush(stdout);
		}

		if (reintentos>RETRIES_TO_RECEIVE_EXPECTED_BYTES){
			rc= read(fd, ((unsigned char*)pff_response)+SIZE_HEADER_FRAME, bytes_en_cola);
			sprintf (msgerror,"Excesivos reintentos para recepcion completa de datos indicados en cabecera");
			//pintartrama(pff_response);
			return -1; // error de lectura
		}
	}
	rc= read(fd, ((unsigned char*)pff_response)+SIZE_HEADER_FRAME, bytes_a_leer);
	if (rc!=bytes_a_leer){
		sprintf(msgerror, "Lectura incompleta en dispositivo puerto serie");
		return -1;
	}


	if (flag_d){
		unsigned char* tramar;
		tramar=(unsigned char *)pff_response;
		for (i=SIZE_HEADER_FRAME;i<18;i++){
			printf("trama respuesta: TRAMA-R[%d]--> %d\n",i, tramar[i]);
		}
	}

	if (flag_d){
		printf("Leidos %d de %d\n",rc,bytes_a_leer);
		printf("Respuesta: lenght data        --> %d\n",pff_response->lenght);
		printf("Respuesta: device             --> %d\n",pff_response->device);
		printf("Respuesta: number             --> %d\n",pff_response->number);
		printf("Respuesta: command            --> %d\n",pff_response->command);
		for (i=0; i<pff_response->lenght; i++){
		  printf("Respuesta: DATA[%d]            --> %d\n",i,pff_response->data_plus_checksum[i]);
		}
		printf("Respuesta: checksum (DATA[%d]) --> %d\n",i,pff_response->data_plus_checksum[i]);
	}

		// Verificacion del checksum
	 	puntero=&pff_response->lenght;
		checksum=0;
		//el checksum es la suma a 8bits de todos los campos del frame
		//menos los 3 bytes de start y el propio checksum

		for(bytes_a_sumar=(pff_response->lenght + 4);bytes_a_sumar>0; bytes_a_sumar--)
		{
			checksum+=*puntero;
			puntero++;
		}
		//se comprueba con el checksum de la trama
		if (pff_response->data_plus_checksum[pff_response->lenght]!=checksum){
			sprintf (msgerror,"Error de checksum, datos recibidos no fiables");
			return -1;
		}

		// trtatamiento del caso especial de error de comando
		if (pff_response->command==0x0e){
			sprintf(msgerror, "Error 0x%x en comando 0x%x\n", pff_response->data_plus_checksum[1],pff_response->data_plus_checksum[0]);
			return -1;
		}

	return EXIT_SUCCESS;
}

int fi_get_version(int fd, unsigned char n_inverter, struct data_response_get_version *versions)
{

	int rc;
	struct data_response_get_version *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta la trama de peticion
	ff_request.lenght=0x00;
	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0x01;
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
		insstr("Error in fi_get_version(): ", msgerror);
		return -1;
	}

	datos_devueltos= (struct data_response_get_version *)&ff_response.data_plus_checksum;

	versions->type_inverter= 	datos_devueltos->type_inverter;
	versions->IFC_Major=		datos_devueltos->IFC_Major;
	versions->IFC_Minor=		datos_devueltos->IFC_Minor;
	versions->IFC_Release=		datos_devueltos->IFC_Release;
	versions->SW_Major=			datos_devueltos->SW_Major;
	versions->SW_Minor=			datos_devueltos->SW_Minor;
	versions->SW_Release=		datos_devueltos->SW_Release;
	versions->SW_Build=			datos_devueltos->SW_Build;

	return 0;
}

int fi_get_power(int fd, unsigned char n_inverter, float *power){

	int rc;
	struct data_response_get_power {
		unsigned char msb;
		unsigned char lsb;
		signed   char exp;
	} *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x00;
	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0x10;
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	// envio de comando y respuesta
	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
			insstr("Error en función fi_get_power: ", msgerror);
			return -1;
	}
	// se verifica que la longitud de la respuesta es correcta
	// de noche el inversor se despierta unos segundos y responde a este comando pero con longitud de datos = 0
	// por ejemplo: 128128128 0 1 1 16  18
	// por tanto el checksum (18) se interpreta como el msb de la potencia y se devuelve
	// una potencia erronea de 4608W

	if (ff_response.lenght!=sizeof(struct data_response_get_power)){
		insstr("Error en longitud de datos de la respuesta del comando 0x10", msgerror);
		*power=0;
		return -1;
	}

	datos_devueltos= (struct data_response_get_power *)&ff_response.data_plus_checksum;
	*power=(256*datos_devueltos->msb+datos_devueltos->lsb)*
			pow(10,datos_devueltos->exp);

	return EXIT_SUCCESS;
}


int fi_get_day_energy(int fd, unsigned char n_inverter, float *daily_energy){

	int rc;
	struct data_response_get_parameter {
		unsigned char msb;
		unsigned char lsb;
		signed   char exp;
	} *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x00;
	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0x12; //Get daily energy command
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	// envio de comando y respuesta
	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
			insstr("Error en función fi_get_power: ", msgerror);
			return -1;
	}
	// se verifica que la longitud de la respuesta es correcta
	// de noche el inversor se despierta unos segundos y responde a este comando pero con longitud de datos = 0
	// por ejemplo: 128128128 0 1 1 16  18
	// por tanto el checksum (18) se interpreta como el msb de la potencia y se devuelve
	// una potencia erronea de 4608W

	if (ff_response.lenght!=sizeof(struct data_response_get_parameter)){
		insstr("Error en longitud de datos de la respuesta del comando 0x10", msgerror);
		*daily_energy=0;
		return -1;
	}

	datos_devueltos= (struct data_response_get_parameter *)&ff_response.data_plus_checksum;
	*daily_energy=(256*datos_devueltos->msb+datos_devueltos->lsb)*
			pow(10,datos_devueltos->exp);

	return EXIT_SUCCESS;
}

int fi_get_dc_voltage(int fd, unsigned char n_inverter, float *dc_voltage){

	int rc;
	struct data_response_get_parameter {
		unsigned char msb;
		unsigned char lsb;
		signed   char exp;
	} *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x00;
	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0x18; //Get DC voltage command
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	// envio de comando y respuesta
	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
			insstr("Error en función fi_get_power: ", msgerror);
			return -1;
	}
	// se verifica que la longitud de la respuesta es correcta
	// de noche el inversor se despierta unos segundos y responde a este comando pero con longitud de datos = 0
	// por ejemplo: 128128128 0 1 1 16  18
	// por tanto el checksum (18) se interpreta como el msb de la potencia y se devuelve
	// una potencia erronea de 4608W

	if (ff_response.lenght!=sizeof(struct data_response_get_parameter)){
		insstr("Error en longitud de datos de la respuesta del comando 0x10", msgerror);
		*dc_voltage=0;
		return -1;
	}

	datos_devueltos= (struct data_response_get_parameter *)&ff_response.data_plus_checksum;
	*dc_voltage=(float)(256*datos_devueltos->msb+datos_devueltos->lsb)*
			pow(10,datos_devueltos->exp);

	return EXIT_SUCCESS;
}

int fi_get_dc_current(int fd, unsigned char n_inverter, float *dc_current){

	int rc;
	struct data_response_get_parameter {
		unsigned char msb;
		unsigned char lsb;
		signed   char exp;
	} *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x00;
	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0x17; //Get DC current command
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	// envio de comando y respuesta
	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
			insstr("Error en función fi_get_power: ", msgerror);
			return -1;
	}
	// se verifica que la longitud de la respuesta es correcta
	// de noche el inversor se despierta unos segundos y responde a este comando pero con longitud de datos = 0
	// por ejemplo: 128128128 0 1 1 16  18
	// por tanto el checksum (18) se interpreta como el msb de la potencia y se devuelve
	// una potencia erronea de 4608W

	if (ff_response.lenght!=sizeof(struct data_response_get_parameter)){
		insstr("Error en longitud de datos de la respuesta del comando 0x10", msgerror);
		*dc_current=0;
		return -1;
	}

	datos_devueltos= (struct data_response_get_parameter *)&ff_response.data_plus_checksum;
	*dc_current=(float)(256*datos_devueltos->msb+datos_devueltos->lsb)*
			pow(10,datos_devueltos->exp);

	return EXIT_SUCCESS;
}


int fi_get_inverter_caps(int fd, unsigned char n_inverter, unsigned char *inverter_caps){
	int rc;
	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x00;

	ff_request.device=0x01;
	ff_request.number=n_inverter;
	ff_request.command=0xBD; // comando para pedir la capacidades de ajustar la potencia del inversor
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

   	// envio de comando y respuesta
	rc= send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
		insstr("Error en función fi_get_inverter_caps: ", msgerror);
		return -1;
	}

	// se apuntan las estructuras al area de datos conrrespondientes de los bufferes de trama
	*inverter_caps= ff_response.data_plus_checksum[0];
	return EXIT_SUCCESS;

}


int fi_set_powerlimit(int fd, unsigned char n_inverter, unsigned char p_rel){

	int rc;
	struct data_request_set_powerlimit {
			unsigned char cmd_id;// codigo de comando de "remote control". Para poner limite de potencia es 0x01
			unsigned char sep1;  // separador =0x7F
			unsigned char p_rel; // porcentaje de la potencia total del inversor donde se pone el limite
			unsigned char res1; // reservado =0x00
			unsigned char sep2; // separador =0x7F
			unsigned char res2; // reservado =0x00
			unsigned char res3; // reservado =0x00
			unsigned char sep3; // separador =0x7F
			unsigned char res4; // reservado =0x00
			unsigned char n_inverter; //numero del inversor al que se dirige el comando

		} *datos_enviados;

	struct data_response_set_powerlimit {
			unsigned char cmd_id; // codigo de comando de "remote control". Para poner limite de potencia es 0x01
			unsigned char sep1; // separador =0x7F
			unsigned char p_rel; // porcentaje de la potencia total del inversor donde se pone el limite
			unsigned char res1; // reservado =0x00
			unsigned char sep2; // separador =0x7F
			unsigned char res2; // reservado =0x00
			unsigned char res3; // reservado =0x00
			unsigned char sep3; // separador =0x7F
			unsigned char res4; // reservado =0x00
			unsigned char n_inverter; //numero del inversor al que se ha dirigido el comando
		} *datos_devueltos;

	// se limpia el buffer para la trama de respuesta
	ff_response.lenght=0x00;
	ff_response.device=0x00;
	ff_response.number=0x00;
	ff_response.command=0x00;
	memset(ff_response.data_plus_checksum, 0x00, sizeof(ff_response.data_plus_checksum));

	// se ajusta y limpia el buffer para la trama de petición
	ff_request.lenght=0x0A; // 0x09 + el numero de inversores a los que se dirige el comando (en nuestro caso "1")
	ff_request.device=0x00; //
	ff_request.number=0x00; // El comando 0x9F es de broadcast
	ff_request.command=0x9F; // comando para ajustar el limite del inversor
	memset(ff_request.data_plus_checksum, 0x00, sizeof(ff_request.data_plus_checksum));

	// se apuntan las estructuras al area de datos conrrespondientes de los bufferes de trama
	datos_enviados = (struct data_request_set_powerlimit *)&ff_request.data_plus_checksum;
	datos_devueltos= (struct data_response_set_powerlimit *)&ff_response.data_plus_checksum;

	datos_enviados->cmd_id=0x01;
	datos_enviados->sep1=0x7F;
	datos_enviados->p_rel=p_rel>100?100:p_rel; // carga valor porcentaje con límite en 100
	datos_enviados->res1=0x00;
	datos_enviados->sep2=0x7F;
	datos_enviados->res2=0x00;
	datos_enviados->res3=0x00;
	datos_enviados->sep3=0x7F;
	datos_enviados->res4=0x00;
	datos_enviados->n_inverter=n_inverter;

	// envio de comando y respuesta
	rc=send_command(fd, &ff_request, &ff_response);
	if (rc==-1){
		insstr("Error en función fi_set_powerlimit: ", msgerror);
		return -1;
	}
	if (datos_devueltos->n_inverter!=0xFF){
		sprintf(msgerror, "Error en función fi_set_powerlimit devolvió valor n_inverter distinto de 0xFF");
		return -1;
	}
	else{
		return datos_devueltos->p_rel;
	}
}

int main(int argc, char *argv[]) {

	int fd=0;
	int rc;

	char ficheroDatosInversor[255]="datosinversor.txt";
	int fdatos; // file descriptor ficehro de datos del inversor
	struct stat file_info; //esctura para información de ficehro de datos de inversor
	char linea[1024+1]; //linea de registro de datos de inversor

	time_t segundo_actual=0;
	time_t segundo_anterior=0;
	int segundos_intervalo;
	struct tm *loc_time;
	char buf[150]; //buffer para string de tiempo

	struct data_response_get_version versions;
	unsigned char inverter_caps;
	int lim_pot=100; // limite de potencia puesto al inversor (en porcentaje de la potencia nominal)
	//float energia_diaria_generada;
	float tension_DC;
	float corriente_DC;
	float energia_diaria_generada_anterior;



	float pot_max,pot_min;// potencia generada maxima y minima en intervalo 15 minuto
	float pot_med=0; // potencia generada media en 15 minuto
	int lim_pot_para_media=0; //acumula los limites (porcentuales) de potencia en el intervalo para luego calcular la media del intervalo (15min, 900 segundos)


	printf ("%s", identificacion);

	int index; //apunta a non-option arguments de getopt()
	int opt;
	int num_inversor=1;
	int control_potencia=0;
	int flag_l = 0; // opcion de limitacion de potencia
	int flag_p = 0; // opcion de declaracion de potencia nominal del inversor

	    // Shut GetOpt error messages down (return '?'):
	    opterr = 0;
	    // Retrieve the options:
	    while ( (opt = getopt(argc, argv, "hi:lp:d")) != -1 ) {  // for each option...
	        switch ( opt ) {
        		case 'd': // identificador de inversor en red RS422
        			flag_d=1;
        			break;
	        	case 'i': // identificador de inversor en red RS422
	        		num_inversor=atoi(optarg);
	        		break;
	       	    case 'l': // limitada potencia generada para no exportar a red
	       	    	flag_l=1;
	       	    	control_potencia = 1;
	       	    	break;
	            case 'p': //potencia nominal del inversor
	            	flag_p=1;
	            	potencia_nominal_inversor = atoi(optarg);
	                break;
	            case 'h': // help
	               	printf("\nUse: fronius-mon [-i num_inv] [[-l] [-p pot_inv]] [-d] [dev_file]");
					printf("\n-i number of inverter in rs422 network/connetion. 1 is the default");
					printf("\n-l limit generating power to avoid export of energy to grid. Requires -p option");
					printf("\n-p nominal power of inverter in watts");
					printf("\n-d display frames for debug");
					printf("\n dev_file device for rs422. Default is /dev/ttyUSB0");
					printf("\n");
					return -1;
	            case '?':  // unknown option...
	            	printf("\nOption -%c invalid. Use -h option for info", optopt);
	            	printf("\n");
	            	return -1;
	        }
	    }

	    if (num_inversor<=0){
	    	printf("\nInvalid inverter number");
	    	return -1;
	    }
	    if (flag_l==1 && flag_p==0 ){
	    	printf("\nOption -l requires option -p");
	    	return -1;
	    }
	    if (potencia_nominal_inversor<=0){
	    	printf("\nInvalid inverter nominal power");
	    	return -1;
	    }

	    for (index = optind; index < argc; index++){
	        portname1=argv[index];
	    }
	    printf("\ndev_file:%s  num_inversor:%d power_limitation:%s  Inverter_nominal_power:%d\n", portname1, num_inversor, control_potencia==1?"true":"false" , potencia_nominal_inversor);

	/*
     * accede o crea area de memoria compartida con medidor de potencia importada
	 */
	int shmid;
	struct datos_publicados *datos_publicados;
	shmid = shmget(SHM_KEY_DATOS_PUBLICADOS, sizeof (struct datos_publicados), IPC_CREAT | 0666);
	datos_publicados = shmat(shmid, NULL, 0);

#if 1
	// Abre fichero datos de inversor y pone cabecera si necesario (fichero vacio)
	fdatos = open(ficheroDatosInversor, O_CREAT|O_APPEND|O_RDWR,S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	fstat (fdatos, &file_info);
	if (file_info.st_size==0){
		sprintf(linea, "dia\x09hora\x09Pot. med\x09energia\x09Pot. max\x09Pot. min\x09limite\n");
	 	write(fdatos, linea, strlen(linea));
	    sprintf(linea, "   \x09    \x09  (w)   \x09 (w*s)  \x09 (w)   \x09  (w)   \x09(porc)\n");
	    write(fdatos, linea, strlen(linea));
	}
#endif

	/*
	 * Temporizador de cada segundo
	 */
	int fd_timer_segundo; //
	fd_timer_segundo = timerfd_create(CLOCK_REALTIME, 0);
	struct timeval tiempo;
	struct itimerspec ts;
	uint64_t numExp=0;
	/*
	 * alinea el inicio del temporizador con el inicio de segundo de tiempo real
	 */

	/* obtiene el tiempo actual en segundos y nanosegundos*/
	 gettimeofday(&tiempo, NULL);
	 /* ajusta salto de temporizador con el siguiente segunto en tiempo absoluto*/
	ts.it_value.tv_sec=tiempo.tv_sec+1;
	ts.it_value.tv_nsec=0;
	/* ajusta salto periodico a 1s */
	ts.it_interval.tv_sec=1;
	ts.it_interval.tv_nsec=0;
	timerfd_settime(fd_timer_segundo, TFD_TIMER_ABSTIME, &ts, NULL);

	int cerrar_ps=0; //señala si puerto serie debe cerrarse (0) o si permanece abierto (1)
	while (1){ //Bucle de apertura

		// se asegura el cierre del puerto serie
		if(cerrar_ps && fd!=0 ){ // nunca cierra el fichero con file descriptor=0, esto es file input, Esto ocurre la primera vez
			close(fd);
			usleep(3000000); //Espera 3 segundos
		}
		// abre fichero de puerto serie
		// para tener permiso si es usuario no root asegurar que pertenece al grupo dialout

		printf ("Opening serial device %s ", portname1);
		fflush(stdout);
		fd = open (portname1, O_RDWR | O_NOCTTY);
		if (fd < 0) {
			printf ("error %d opening %s: %s\n", errno, portname1, strerror (errno));
			fflush(stdout);
			cerrar_ps=0; // el puerto serie no ha llegado a abrirse
			sleep(5); // espera 5ds
			continue;
		}
		else {
			cerrar_ps=0;
			configura_puerto_serie(fd);
			printf ("fd:%d. Listening inverter %d", fd, num_inversor);

			// obtiene datos de inversor
			rc=fi_get_version(fd, num_inversor, &versions);
			if (rc==-1){
				printf("%s\r", msgerror);
				cerrar_ps=1;
				continue;
			}
			else {
				printf("Serie inversor: %d, version IFC:%d.%d.%d Version SW:%d.%d.%d.%d\n",
						versions.type_inverter,
						versions.IFC_Major, versions.IFC_Minor,versions.IFC_Release,
						versions.SW_Major, versions.SW_Minor, versions.SW_Release, versions.SW_Build);
				rc=fi_get_inverter_caps(fd, num_inversor, &inverter_caps);
				if (rc==-1)				{
					printf("Error en fi_get_invertercaps:%s\n", msgerror);
					cerrar_ps=1;
					continue;
				}
				else {
					if((inverter_caps & 0x01)==0){
						printf("Inversor NO capacitado para aceptar comandos de reduccion de potencia\n");
					}
					else{
						printf("Inversor capacitado para aceptar comandos de reduccion de potencia\n");
						lim_pot=100;
						rc=fi_set_powerlimit(fd, num_inversor,lim_pot); // asegura que inicialmente está al 100%
						if (rc==-1){
							printf("Error en fi_set_powerlimit:%s\n", msgerror);
							cerrar_ps=1;
							continue;
						}
					}
				}
			}
		}

		//se espera al vencimiento del temporizador se obtiene y guarda la energia inicial y su tiempo
		read(fd_timer_segundo, &numExp, sizeof(uint64_t));
		segundo_anterior= time(NULL);
		rc=fi_get_day_energy(fd, num_inversor, &datos_publicados->energia_generada_dia);
		if (rc==-1){
			printf("Error en fi_get_day_energy:%s\n", msgerror);
			cerrar_ps=1;
			continue;
		}
		energia_diaria_generada_anterior=datos_publicados->energia_generada_dia;
		pot_max=0;
		pot_min=FLT_MAX;
		pot_med=0;
		lim_pot_para_media=0;



		while (1){ // bucle de lectura y ajuste de potencia

			/*
			* la ejecucion queda suspendida en la función read() hasta que el temporizador se dispare (alcance el nuevo segundo)
			*/
			read(fd_timer_segundo, &numExp, sizeof(uint64_t));
			//  se toma el tiempo
			segundo_actual = time(NULL);
			loc_time = localtime (&segundo_actual); // Converting current time to local time

			// acciones cada 15m
			if (loc_time->tm_min%15==0){

				energia_diaria_generada_anterior = datos_publicados->energia_generada_dia;
				segundo_anterior=segundo_actual;
				pot_max=0;
				pot_min=FLT_MAX;
				lim_pot_para_media=0;
			}

			rc=fi_get_power(fd, num_inversor, &datos_publicados->potencia_generada);
			if (rc==-1){
				printf("Error en fi_get_power:%s\n", msgerror);
				datos_publicados->potencia_generada=0;
				cerrar_ps=1;
				break;
			}

			//TODO quitar esta variable. Emplear datos_instantaneos->potencia
			int potencia_importada;
			potencia_importada=datos_publicados->potencia_consumo - datos_publicados->potencia_generada;

			if (control_potencia==1){
				if (potencia_importada<0 ){
					lim_pot=(datos_publicados->potencia_consumo*100)/potencia_nominal_inversor;
					lim_pot=lim_pot<10?10:lim_pot;  //evita poner limite por debajo del 10% para evitar parada de inversor
				}
				else{ //incremento lento (1%) de la potencia generada hasta llegar a 100%
					lim_pot=lim_pot>=100-1?100:lim_pot+1;
				}
				rc=fi_set_powerlimit(fd, num_inversor,lim_pot);
				if (rc==-1){
					printf("Error en fi_set_powerlimit:%s\n", msgerror);
					cerrar_ps=1;
					break;
				}
			}

			rc=fi_get_day_energy(fd, num_inversor, &datos_publicados->energia_generada_dia);
			if (rc==-1){
				printf("Error en fi_get_day_energy:%s\n", msgerror);
				cerrar_ps=1;
				break;
			}

			rc=fi_get_dc_voltage(fd, num_inversor, &tension_DC);
			if (rc==-1){
				printf("Error en fi_get_dc_voltage:%s\n", msgerror);
				cerrar_ps=1;
				break;
			}
			rc=fi_get_dc_current(fd, num_inversor, &corriente_DC);
			if (rc==-1){
				printf("Error en fi_get_dc_current:%s\n", msgerror);
				cerrar_ps=1;
				break;
			}

			pot_max=datos_publicados->potencia_generada>pot_max?datos_publicados->potencia_generada:pot_max;
			pot_min=datos_publicados->potencia_generada<pot_min?datos_publicados->potencia_generada:pot_min;
			lim_pot_para_media+=lim_pot;

			strftime (buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", loc_time);
			printf("\r%s Pot gen.: %5.1fW  Lim gen.: %5dW  Pot imp.: %5dW  Pot con.: %5.1fW  Energia diaria: %5.1fWh  DC: %3.1fV %.3fA %5.1fW",
					buf,
					datos_publicados->potencia_generada,
					(lim_pot*potencia_nominal_inversor)/100,
					potencia_importada,
					datos_publicados->potencia_consumo,
					datos_publicados->energia_generada_dia,
					tension_DC,
					corriente_DC,
					(tension_DC*corriente_DC)
					);

			fflush(stdout);

			int intervalo_15min;
			intervalo_15min=loc_time->tm_hour*4+(loc_time->tm_min/15);
			datos_publicados->entradaregistrodiario[intervalo_15min].energia_generada=datos_publicados->energia_generada_dia-energia_diaria_generada_anterior;

			segundos_intervalo = segundo_actual - segundo_anterior;
			if (segundos_intervalo > 0){
				pot_med = datos_publicados->entradaregistrodiario[intervalo_15min].energia_generada * 3600 / segundos_intervalo;
				lim_pot_para_media=lim_pot_para_media/segundos_intervalo;
			}

			// Registrar cuando las lecturas completan un minuto
			//la segunda condicion es para evitar que el primer tiempo tras arrancar el programa coincida con segundo=0 lo que provoca una division por 0.
			if (loc_time->tm_sec==0 && segundo_anterior!=0){
				sprintf(linea, "%s %4.1f %6.1f %3d %4.1f %4.1f %3d\n", buf, pot_med, datos_publicados->entradaregistrodiario[intervalo_15min].energia_generada, segundos_intervalo,  pot_max, pot_min, lim_pot_para_media);
				printf("\n%s", linea);
				write(fdatos, linea, strlen(linea));
				printf("intervalo_15min:%d energia gen:%5.1f energia con:%5.1f \n",
						intervalo_15min,
						datos_publicados->entradaregistrodiario[intervalo_15min].energia_generada,
						datos_publicados->entradaregistrodiario[intervalo_15min].energia_consumida);

			}
		} // final bucle de lecturas y ajuste potencia
	}// fin bucle de apertura
	return EXIT_SUCCESS;
}

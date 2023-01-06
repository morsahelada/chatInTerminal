extern "C"{
    #include <unistd.h>
    #include <pthread.h>

    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <arpa/inet.h>

    #include <string.h>

    #include <signal.h>
}

#include <iostream>

#define CONNECT 0
#define DISCONNECT 1

using namespace std;

/*
* 
* Se recomienda leer los comentarios desde main() hacia adelante para una mejor comprensión.
* La función de cada variable será descrita en los comentarios a parir de main.
*
*/

/*Estructuras*/

struct Event; //Estructura que se usa en los eventos de connect y disconnect.
struct FreeThread; //Estructura que se usa para almacenar los hilos libres en el stack.

/*Varables necesariamente globales*/

int sockfd; //Fd del socket.

Event *stackEvent = NULL; //El apuntador a la cumbre del stack de eventos.

FreeThread *stackFreeThreads = NULL; //El apuntadr a la cumbre del stack de hilos libres.

/*Prototipo de funciones*/

void *threadRoutine(void*); //Rutina del hilo que conrola una conexión en espcífico.

void pushEvent(Event); //Función push del stack de eventos.
void popEvent(Event &); //Función pop del stack de eventos.

void pushFreeThread(int threadID); //Función push del stack de hios libres.
void popFreeThread(int &ThreadID); //Función pop del stack de hilos libres.

/*Declaración de las estructuas*/

struct Event{
    int threadID; //La posición del hilo que ha creado el evento en el array de hilos.
    int type; //El tipo de evento, 1. CONNECT, 2. DISCONNECT.
    Event *before; //Apuntador al elemento anterior del stack.
};

struct FreeThread{
    int threadID; //La posicion del hilo libre en el array de hilos.
    FreeThread *before; //Apuntador al elemento anterior del stack.
};

/*Funciones extra*/

void errorColor(){
    cout<<"\x1b[31;40m";
}
void normalColor(){
    cout<<"\x1b[37;40m";
}

/*Funcion prinipal.*/

int main(int argc, char *argv[]){
    /*Variables necesarias de main.*/

    //Variables de los argumentos.

    int maxClients = 10; //Maximo de cliente conectados simultaneamnte; Por defecto: 10.
    int port = 27279; //Puerto del servidor; Por defecto: 27279.
    char *address = "192.168.0.19"; //Dirección del servidor; Por defecto: 192.168.0.19.

    //Variables para el manejo de las conexiones.

    pthread_t *threads; //Arreglo declarado como un puntero. Almacena el id de cada hilo de cada conexión.

    //Variables del socket del servidor no globales.

    sockaddr_in serverAddress;

    /*
    *
    * Establecer argumentos de entrada del programa.
    *
    * Los argumentos són: Maxiomo de clientes(maxClients), puerto(port) y dirección(address).
    * 
    */
    
    if(argc > 1){
        maxClients = atoi(argv[1]);
    }
    if(argc > 2){
        port = atoi(argv[3]);
    }
    if(argc > 3){
        address = argv[2];
    }

    //Inicialización de las variables declaradas.

    threads = new pthread_t[maxClients]; //El tamaño del arreglo sere el maximo de conexiones; cada conexión se trabaja en un hilo.

    //Iicializar el socket

    sockfd = socket(AF_INET, SOCK_STREAM, 0);;

    if(sockfd == -1){
        errorColor();
        cout<<"[Server-Error]: "<<"No se ha podido inicializar el socket del servidor."<<endl;
        normalColor(); 
        delete threads;
        return -1;
    }

    cout<<"[Sever]: "<<"Socket del servidor creado satisfactoriamente."<<endl;

    const int enabled = 1; //Variable necesaria para establecer el SO_REUSEADDR
    if(setsockopt(sockfd,  SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(int)) < 0){
        errorColor();
        cout<<"[Server-Error]: "<<"Error configurando: 'SO_REUSEADDR'."<<endl;
        normalColor();
        close(sockfd);
        delete threads;
        return -1;
    }

    memset(&serverAddress, 0, sizeof(serverAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = inet_addr(address);
    serverAddress.sin_port = htons(port);

    if(bind(sockfd, (sockaddr *)&serverAddress, sizeof(serverAddress)) != 0){
        errorColor();
        cout<<"[Server-Error]: "<<"Error al ligar el socke del servidor con la dirección."<<endl;
        normalColor();
        close(sockfd);
        delete threads;
        return -1;
    }

    cout<<"[Server]: "<<"Socket del servidor ligado con la direccion satisfactoriamente."<<endl;

    if(listen(sockfd, 65535) != 0){
        errorColor();
        cout<<"[Server-Error]: "<<"Error al escuchar en el puerto: "<<port<<"."<<endl;
        normalColor();
        close(sockfd);
        delete threads;
        return -1;
    }

    cout<<"[Server]: "<<"Servidor escuchando en el puerto: "<<port<<"."<<endl;

    //Socket inicializado.

    /*
    *
    * Explicación I:
    * 1. El primer hilo del array de hilos inicia y establece la conexión con el cliente.
    * 2. Cuando el cliente se conecte con el primer hilo, se ejecutará el siguiente hilo que esperará la conexión de un nuevo cliente.
    * 3. Si se establece una conexión en el hilo que espera las conexiones, se ejecutará el proceso 2.
    * 4. Si un cliente se desconecta, la posició del hilo en el array se almacenara en un stack para ser usada nuevamente.
    * 
    * Siempre hay un hilo esperando a una conexión.
    * A la hora de ejecutar un nuevo hilo, el hilo se ejecutara en las posiciones del array almacenadas en el stack antes que en otras.
    * 
    */

    Event *lastEventLoaded = stackEvent; //Puntero al ultimo evento ejecutado del stack.
    Event event; //Estructura Event, almacena los datos de el evento que se está procesando.
    int threadWaiting = 0; //Almacena la posicion del hilo que espera una conexión en el array de hilos.

    //Inicia el primer hilo del array.

    pthread_create(&threads[0], NULL, threadRoutine, &threadWaiting);

    while(1){
        /*
        *
        * Explicación de los eventos:
        * 1. Cuando una conexión es aceptada por un hilo o se cierra una conexión, se añade un elemento al stackEvent.
        * Cada Event en el stackEvent contiene la posición del hilo en el array de hilos, el tipo de evento: 1 CONNECT, 2 DISCONNECT; y un apuntador al anterior elemento del stack.
        * 2. Cada vez que se ejecuta este bucle, se comprueba si hay un evento en el stack de eventos.
        * 3. Si hay algun evento, se ejecuta la acción necesaria, si no, se vuelve a ejecutar el bucle.
        * 
        * Cada vez que un cliente se conecta o se desconecta, se añde un evento al stack.
        * Se ejecuta la acción de cada evento basandse en la Explicación I.
        *
        */

        if(lastEventLoaded != stackEvent){
            popEvent(event);
            switch(event.type){
                case CONNECT:
                    if(stackFreeThreads != NULL){ //Si hay elementos en el stack de hilos libres, se dara prioridad a esos hilos libres del array.
                        int threadIDtmp;
                        popFreeThread(threadIDtmp);
                        pthread_create(&threads[threadIDtmp], NULL, threadRoutine, &threadIDtmp);
                        break;
                    }
                    
                    //Creamos el nuevo hilo.
                    pthread_create(&threads[threadWaiting + 1], NULL, threadRoutine, &threadWaiting);
                    threadWaiting++; //El hilo que espera es el siguiente al anterior. 
                    break;
                case DISCONNECT:
                    pushFreeThread(event.threadID); //Si un hilo se cierra, se agrega al stack de hilos librs para ser usado con prioridad.
                    break;
            }
        }       
    }

    return 0;
}

void *threadRoutine(void *arg){

}

/*Declaración de funciones del stack de eventos*/

void pushEvent(Event event){
    //Un codiggo simple para añadir elementos a el stackEvent.
    Event *newEvent = new Event;
    *newEvent = event;
    newEvent->before = stackEvent;
    stackEvent = newEvent;
}

void popEvent(Event &event){
    //Un código simple para extraer elementos del stackEvent.
    Event *tmpEvent = stackEvent;
    event = *tmpEvent;
    stackEvent = tmpEvent->before;
    delete tmpEvent;
}


void pushFreeThread(int threadID){
    //Un codigo simple ara añadir elementos al stackFreeThreads.
    FreeThread *newFreeThread = new FreeThread;
    newFreeThread->threadID = threadID;
    newFreeThread->before = stackFreeThreads;
    stackFreeThreads = newFreeThread;
}

void popFreeThread(int &threadID){
    //Un codigo simple ara extraer elementos al stackFreeThreads.
    FreeThread *tmpFreeThread = stackFreeThreads;
    threadID = tmpFreeThread->threadID;
    stackFreeThreads = tmpFreeThread->before;
    delete tmpFreeThread;
}
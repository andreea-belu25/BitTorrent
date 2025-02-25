#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

typedef struct {
    char *filename;
    int noOfOwnedSeg;
    char **ownedSeg;
} FileInfoPeer;

typedef struct {
    int client;
    int stress;
} ClientStress;

typedef struct {
    int noOfOwnedFiles;
    FileInfoPeer *ownedFiles;
    int noOfWantedFiles;
    char **wantedFiles;
    int rank;
} PeerData;

typedef struct {
    int position;
    char *hash;
} Segment;

typedef struct {
    char *filename;
    Segment *segHashes;
    int noOfSeg;
} FileInfoTracker;

typedef struct {
    int *clients;
    int noOfClients;
} Swarm;

typedef struct {
    int noOfFiles;
    FileInfoTracker *files;
    Swarm *swarms;
} TrackerData;

//  when the file is completely downloaded, save it
void saveFile(int rank, char* wantedFile, char** segHashes, int noOfSeg) {
    char* outputFile = malloc(2 * MAX_FILENAME * sizeof(char));
    sprintf(outputFile, "client%d_%s", rank, wantedFile);
    
    FILE* file = fopen(outputFile, "w");
    if (!file) {
        printf("ERROR: Something happend with writing in the file!\n");
        return;
    }

    for (int i = 0; i < noOfSeg; i++)
        fprintf(file, "%s\n", segHashes[i]);

    fclose(file);
}

//  get the swarm for a specific file from the tracker
Swarm *getSwarmForFile(char *wantedFile) {
    MPI_Status status;
    Swarm *swarm = malloc(sizeof(Swarm));

    MPI_Send(wantedFile, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 3, MPI_COMM_WORLD);
    
    MPI_Recv(&swarm->noOfClients, 1, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, &status);
    swarm->clients = malloc(swarm->noOfClients * sizeof(int));
    MPI_Recv(swarm->clients, swarm->noOfClients, MPI_INT, TRACKER_RANK, 3, MPI_COMM_WORLD, &status);

    return swarm;
}

//  for the efficiency part - sort the clients after their levels of stress, otherwise after their ranks
int sortClients(const void *a, const void *b) {
    const ClientStress *client1 = (const ClientStress *)a;
    const ClientStress *client2 = (const ClientStress *)b;
    if (client1->stress != client2->stress)
        return (client1->stress - client2->stress);
    
    return client1->client - client2->client;
}

void *download_thread_func(void *arg) {
    PeerData *peerData = (PeerData *)arg;
    
    for (int i = 0; i < peerData->noOfWantedFiles; i++) {
        char *wantedFile = peerData->wantedFiles[i];
        MPI_Status status;
        int noOfSeg;

        //  ask and receive hashes from the tracker
        MPI_Send(wantedFile, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 4, MPI_COMM_WORLD);
        MPI_Recv(&noOfSeg, 1, MPI_INT, TRACKER_RANK, 4, MPI_COMM_WORLD, &status);
        char **segmentHashes = calloc(noOfSeg, sizeof(char *));
        for (int seg = 0; seg < noOfSeg; seg++) {
            segmentHashes[seg] = calloc((HASH_SIZE + 1), sizeof(char));
            MPI_Recv(segmentHashes[seg], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 4, MPI_COMM_WORLD, &status);
        }

        //  init the information for the file
        int noOfOwnedFiles = peerData->noOfOwnedFiles + 1;
        peerData->ownedFiles = realloc(peerData->ownedFiles, noOfOwnedFiles * sizeof(FileInfoPeer));
		peerData->ownedFiles[noOfOwnedFiles - 1].filename = calloc(MAX_FILENAME, sizeof(char));
		for (int j = 0; j < MAX_FILENAME; j++)
			peerData->ownedFiles[noOfOwnedFiles - 1].filename[j] = wantedFile[j];
		peerData->ownedFiles[noOfOwnedFiles - 1].noOfOwnedSeg = 0; // initially no segment is available
		peerData->ownedFiles[noOfOwnedFiles - 1].ownedSeg = segmentHashes;

		peerData->noOfOwnedFiles = noOfOwnedFiles;
		
        Swarm *swarm;
        ClientStress *stressList;
        for (int j = 0; j < noOfSeg; j++) {
            if (j % 10 == 0) {  //  get the swarm after each 10 sent segments
                if (swarm)
                    free(swarm);
                if (stressList)
                    free(stressList);

                swarm = getSwarmForFile(wantedFile);

                //  create a list of clients considering their levels of stress
                stressList = calloc(swarm->noOfClients, sizeof(ClientStress));
                for (int k = 0; k < swarm->noOfClients; k++) {
                    stressList[k].client = swarm->clients[k];
                    stressList[k].stress = 0;
                }
            }

            //  sort the list of clients for efficiency
            qsort(stressList, swarm->noOfClients, sizeof(ClientStress), sortClients);
            //  start finding the missing hash and stop when a client can provide it
            for (int k = 0; k < swarm->noOfClients; k++) {
                int peer = stressList[k].client;
                if (peer != peerData->rank) {
                    MPI_Send(segmentHashes[j], HASH_SIZE + 1, MPI_CHAR, peer, 8, MPI_COMM_WORLD);
                    MPI_Send(wantedFile, MAX_FILENAME, MPI_CHAR, peer, 8, MPI_COMM_WORLD);

                    int response = -1;
                    MPI_Recv(&response, 1, MPI_INT, peer, 9, MPI_COMM_WORLD, &status);
                    if (response == 1) {
                        stressList[k].stress += 1;
						peerData->ownedFiles[noOfOwnedFiles - 1].noOfOwnedSeg = j + 1; // mark segment as available
						break; // segment found
                    }
                }
            }
        }

        //  finished downloading the entire file => save the file
        saveFile(peerData->rank, wantedFile, segmentHashes, noOfSeg);
    }
    
    //  inform the tracker that the client is done
	char *buffer = calloc(MAX_FILENAME, sizeof(char));
	char finished[] = "finished";
	strcpy(buffer, finished);
    MPI_Send(finished, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 6, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg) {
    PeerData *peerData = (PeerData*) arg;
    MPI_Status status;
    
    char *buffer = calloc(HASH_SIZE + 1, sizeof(char));
    while (1) {
        MPI_Recv(buffer, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, 8, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;
        if (strcmp(buffer, "close") == 0)
            break; // close signal
        
        char *filename = calloc(MAX_FILENAME, sizeof(char));
        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, source, 8, MPI_COMM_WORLD, &status);

        //  start looking after the requested segment
        int foundSeg = 0;
        for (int i = 0; i < peerData->noOfOwnedFiles && foundSeg == 0; i++) {
            FileInfoPeer currentFile = peerData->ownedFiles[i];
            if (strcmp(currentFile.filename, filename) == 0) {
                
                for (int j = 0; j < currentFile.noOfOwnedSeg; j++) {
                    if (strcmp(buffer, currentFile.ownedSeg[j]) == 0) {
                        foundSeg = 1;  //  stop searching if the segment was already found
                        break;
                    }
                }
            }
        }
        
        MPI_Send(&foundSeg, 1, MPI_INT, source, 9, MPI_COMM_WORLD); // segment is not available
    }

    return NULL;
}

void reqFileSwarm(TrackerData *trackerData, char *filename, int source) {
    int fileIdx;

    for (int i = 0; i < trackerData->noOfFiles; i++) {
        fileIdx = i;
        if (strcmp(filename, trackerData->files[i].filename) == 0) {
            MPI_Send(&trackerData->swarms[i].noOfClients, 1, MPI_INT, source, 3, MPI_COMM_WORLD);
            MPI_Send(trackerData->swarms[i].clients, trackerData->swarms[i].noOfClients, MPI_INT, source, 3, MPI_COMM_WORLD);
            break;
        }
    }

    //  check if the client is already in the swarm of the file
	int exists = 0;
	for (int i = 0; i < trackerData->swarms[fileIdx].noOfClients; i++) {
		if (trackerData->swarms[fileIdx].clients[i] == source)
			exists = 1;
	}

    //  add it otherwise
	if (exists == 0) {
		int pos = trackerData->swarms[fileIdx].noOfClients;
		trackerData->swarms[fileIdx].noOfClients++;
		trackerData->swarms[fileIdx].clients[pos] = source;
	}
}

void reqFileInfo(TrackerData *trackerData, char *filename, int source) {
    for (int i = 0; i < trackerData->noOfFiles; i++) {
        //  find the requested file
        if (strcmp(filename, trackerData->files[i].filename) != 0)
            continue;

        //  send the requested information about it
        MPI_Send(&trackerData->files[i].noOfSeg, 1, MPI_INT, source, 4, MPI_COMM_WORLD);
        for (int j = 0; j < trackerData->files[i].noOfSeg; j++)
            MPI_Send(trackerData->files[i].segHashes[j].hash, HASH_SIZE + 1, MPI_CHAR, source, 4, MPI_COMM_WORLD);
        break;
    }
}

void finishedDownloading(int completedClients, int numtasks) {
    char *buffer = calloc(HASH_SIZE + 1, sizeof(char));
    char close[] = "close";
    strcpy(buffer, close);
    //  notify all the clients that downloading is over
    for (int client = 1; client < numtasks; client++) 
        MPI_Send(buffer, HASH_SIZE + 1, MPI_CHAR, client, 8, MPI_COMM_WORLD);  //  stop each client
}

void handleMessages(TrackerData* trackerData, int numtasks) {
    char* filename = malloc(MAX_FILENAME * sizeof(char));
    MPI_Status status;
    int finished_clients = 0;

    while (1) {
        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;

        if (tag == 3) {  //  request for a file/ request for actualization
            reqFileSwarm(trackerData, filename, source);
        } else if (tag == 4) {
            reqFileInfo(trackerData, filename, source);  //  request for information about the file
        } else if (tag == 5) {  //  file completely downloaded for a client
        } else if (tag == 6) {  //  all files completely downloaded for a client
            finished_clients++;        
            if (finished_clients == numtasks - 1) { //  all clients finished downloading all their wanted files
                break;  //  stop the tracker
            }
        }
    }

    //  inform the clients that downloading is over
    finishedDownloading(finished_clients, numtasks);
}

int checkIfFileExists(TrackerData *trackerData, char *filename) {
    for (int filePos = 0; filePos < trackerData->noOfFiles; filePos++)
        if (strcmp(trackerData->files[filePos].filename, filename) == 0)
            return filePos;
    
    return -1;
}

void addClientIfNotExistsSwarm(TrackerData *trackerData, int client, int filePos) {
    int clientFound = 0;

    for (int i = 0; i < trackerData->swarms[filePos].noOfClients; i++) {
        if (trackerData->swarms[filePos].clients[i] == client) {
            clientFound = 1;
            break;
        }
    }
    
    if (clientFound == 0) {
        int pos = trackerData->swarms[filePos].noOfClients;
        trackerData->swarms[filePos].noOfClients++;
        trackerData->swarms[filePos].clients[pos] = client;
    }
}

void initFile(TrackerData *trackerData, int filePos, int client, int noOfSegments, char *filename) {
    MPI_Status status;

    trackerData->files[filePos].filename = filename;
    trackerData->files[filePos].noOfSeg = noOfSegments;
    trackerData->files[filePos].segHashes = malloc(noOfSegments * sizeof(Segment));

    for (int seg = 0; seg < noOfSegments; seg++) {
        char *hash = malloc((HASH_SIZE + 1) * sizeof(char));
        MPI_Recv(hash, HASH_SIZE + 1, MPI_CHAR, client, 1, MPI_COMM_WORLD, &status);
        trackerData->files[filePos].segHashes[seg].hash = hash;
        trackerData->files[filePos].segHashes[seg].position = seg;
    }
}

void initSwarm(TrackerData *trackerData, int filePos, int numtasks) {
    trackerData->swarms[filePos].clients = malloc(numtasks * sizeof(int));
    trackerData->swarms[filePos].noOfClients = 0;
}

//  clients can start uploading and downloading
void startClients(int numtasks) {
    int ack = 1;

    for (int client = 1; client < numtasks; client++)
        MPI_Send(&ack, 1, MPI_INT, client, 2, MPI_COMM_WORLD);
}

void receiveFilesFromClients(TrackerData *trackerData, int numtasks) {
    //  initialize info for the tracker
    trackerData->noOfFiles = 0;
    trackerData->files = malloc(MAX_FILES * sizeof(FileInfoTracker));
    trackerData->swarms = malloc(MAX_FILES * sizeof(Swarm));

    //  start receiving info from clients
    for (int client = 1; client < numtasks; client++) {
        int noOfFiles;
        MPI_Status status;
        MPI_Recv(&noOfFiles, 1, MPI_INT, client, 1, MPI_COMM_WORLD, &status);
        
        for (int file = 0; file < noOfFiles; file++) {
            char *filename = malloc(MAX_FILENAME * sizeof(char));
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, client, 1, MPI_COMM_WORLD, &status);

            int filePos = checkIfFileExists(trackerData, filename);

            if (filePos == -1) {
                filePos = trackerData->noOfFiles;

                int noOfSegments;
                MPI_Recv(&noOfSegments, 1, MPI_INT, client, 1, MPI_COMM_WORLD, &status);

                initFile(trackerData, filePos, client, noOfSegments, filename);
                initSwarm(trackerData, filePos, numtasks);

                trackerData->noOfFiles++;
            } else { //  because we still receive the hashes even though we don't need them
                int noOfSegments;
                MPI_Recv(&noOfSegments, 1, MPI_INT, client, 1, MPI_COMM_WORLD, &status);
                for (int seg = 0; seg < noOfSegments; seg++) {
                    char *hash = malloc((HASH_SIZE + 1) * sizeof(char));
                    MPI_Recv(hash, HASH_SIZE + 1, MPI_CHAR, client, 1, MPI_COMM_WORLD, &status);
                }
            }

            addClientIfNotExistsSwarm(trackerData, client, filePos);
        }
    }

    startClients(numtasks);
}

void sendFilesToTracker(int rank, PeerData *peerData) {
    //  send to the tracker the number of owned files
    MPI_Send(&peerData->noOfOwnedFiles, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);

    /*  for each ownedFile send to the tracker its filename,
        the number of segments and for each segment its corresponding hash
    */
    for (int i = 0; i < peerData->noOfOwnedFiles; i++) {
        MPI_Send(peerData->ownedFiles[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
        MPI_Send(&peerData->ownedFiles[i].noOfOwnedSeg, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
        for (int j = 0; j < peerData->ownedFiles[i].noOfOwnedSeg; j++) {
            MPI_Send(peerData->ownedFiles[i].ownedSeg[j], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
        }
    }   

    //  wait for the tracker to acknowledge
    int res;
    MPI_Status status;
    MPI_Recv(&res, 1, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD, &status);
}

void readFile(int rank, PeerData *peerData) {
    char *filename = calloc(50, sizeof(char));
    char path[] = "in";
    strncpy(filename, path, strlen(path));
    char *rankStr = calloc(2, sizeof(char));
    rankStr[0] = rank + '0';
    rankStr[1] = '\0';
    strncat(filename, rankStr, strlen(rankStr));
    strcat(filename, ".txt");
    filename[strlen(filename)] = '\0';
    
    FILE *file = fopen(filename, "r");

    if (!file) {
        printf("ERROR: Something happened with opening the file!\n");
        return;
    }
    
    fscanf(file, "%d", &peerData->noOfOwnedFiles);
    peerData->ownedFiles = calloc(peerData->noOfOwnedFiles, sizeof(FileInfoPeer));

    for (int i = 0; i < peerData->noOfOwnedFiles; i++) {
        peerData->ownedFiles[i].filename = calloc(MAX_FILENAME, sizeof(char));
        fscanf(file, "%s", peerData->ownedFiles[i].filename);
        fscanf(file, "%d", &peerData->ownedFiles[i].noOfOwnedSeg);
        peerData->ownedFiles[i].ownedSeg = malloc(peerData->ownedFiles[i].noOfOwnedSeg * sizeof(char *));

        for (int j = 0; j < peerData->ownedFiles[i].noOfOwnedSeg; j++) {
            peerData->ownedFiles[i].ownedSeg[j] = malloc(HASH_SIZE * sizeof(char));
            fscanf(file, "%s", peerData->ownedFiles[i].ownedSeg[j]);
        }
    }

    fscanf(file, "%d", &peerData->noOfWantedFiles);

    peerData->wantedFiles = malloc(peerData->noOfWantedFiles * sizeof(char *));

    for (int i = 0; i < peerData->noOfWantedFiles; i++) {
        peerData->wantedFiles[i] = calloc(MAX_FILENAME, sizeof(char));
        fscanf(file, "%s", peerData->wantedFiles[i]);
    }

    peerData->rank = rank;

    fclose(file);
}

void tracker(int numtasks, int rank) {
    TrackerData trackerData;

    receiveFilesFromClients(&trackerData, numtasks);
    handleMessages(&trackerData, numtasks);
}

void peer(int numtasks, int rank) {
    PeerData peerData;

    readFile(rank, &peerData);
    sendFilesToTracker(rank, &peerData);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&peerData);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&peerData);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

int main(int argc, char *argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }

    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
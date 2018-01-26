#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <omp.h> 
#include <R.h>
#include <headers.h>

void sumGrad(double* grad, float* gradA, float* gradB, int n) {
	int i;
	for (i=0; i < n; i++) {
		grad[i] = (double) (gradA[i] + gradB[i]);
	}
}

void calcGradFloatAVXCaller(float *X, float* XW, float *grid, float* a, float* b, float gamma, float weight, float* delta, int n, int dim, int nH, int M, float* gradA, float* gradB, float* TermA, float* TermB, double* influence, unsigned short int* YIdx) {
    
    for (int i = 0; i < nH; i++) {
        influence[i] = 0;
    }
    int modn, modM;
    modn = n%8; modM = M%8;
	
	// set gradients to zero
	memset(gradA,0,nH*(dim+1)*sizeof(float));
	memset(gradB,0,nH*(dim+1)*sizeof(float));
	// set TermA and TermB to zero
	*TermA = 0; *TermB = 0;

	// perform AVX for most entries except the one after the last devisor of 8
    calcGradFullAVXC(gradA,gradB,influence,TermA,TermB,X,XW,grid,YIdx,a,b,gamma,weight,delta,n,M,dim,nH);
	calcGradFloatC(gradA,gradB,influence,TermA,TermB,X + n - modn,XW + n - modn,grid,YIdx + (M - modM)*dim,a,b,gamma,weight,delta,n,modn,modM,dim,nH);
}

/* newtonBFGLSInitC
 *
 * Input: 	float* X			the samples
 * 			float* XW			sample weights
 * 			float* paramsInit	initial parameter vector
 * 			int dim				dimension of X
 * 			int lenP			size of paramsInit
 * 			int n				number of samples
 * */

void newtonBFGSLInitCTest(double* X,  double* XW, double* box, double* params, int *dim_, int *lenP_, int *n_, double* ACVH, double* bCVH, int *lenCVH_, double *intEps_, double *lambdaSqEps_, double* logLike) {

	Rprintf("dim: %d, lenP: %d, N: %d, lenCVH: %d\n",*dim_,*lenP_, *n_, *lenCVH_);
	Rprintf("intEps: %.4e, lambdaSqEps: %.4e\n",*intEps_, *lambdaSqEps_);
}

void newtonBFGSLInitC(double* X,  double* XW, double* box, double* params, int *dim_, int *lenP_, int *n_, double* ACVH, double* bCVH, int *lenCVH_, double *intEps_, double *lambdaSqEps_, double* logLike) {

	omp_set_num_threads(omp_get_max_threads());	
		
	// cast R pointers
	int dim = *dim_, lenP = *lenP_, n = *n_, lenCVH = *lenCVH_;
   	double intEps = *intEps_, lambdaSqEps = *lambdaSqEps_;	
	int i;
	// number of hyperplanes
	int nH  = (int) lenP/(dim+1);

	// create the integration grid
    int lenY, numBoxes = 0;
	int *numPointsPerBox; unsigned short int *YIdx, *XToBox; double *boxEvalPoints;

	// obtain grid density params
	int NGrid, MGrid;
    double weight = 0; 
    double *grid = NULL;
    setGridDensity(box,dim,1,&NGrid,&MGrid,&grid,&weight);
	float *delta = malloc(dim*sizeof(float));
	for (i=0; i < dim; i++) {
		delta[i] = grid[NGrid*MGrid*i+1] - grid[NGrid*MGrid*i];
	}
	//printf("Obtain grid for N = %d and M = %d\n",NGrid,MGrid);
	makeGridC(X,&YIdx,&XToBox,&numPointsPerBox,&boxEvalPoints,ACVH,bCVH,box,&lenY,&numBoxes,dim,lenCVH,NGrid,MGrid,n);
	//printf("Obtained grid with %d points\n",lenY);
	
	// only the first entry in each dimension is required
	float *gridFloat = malloc(dim*sizeof(float));
	for (i=0; i < dim; i++) {
		gridFloat[i] = grid[i*NGrid*MGrid];
	}
	// two points for a and b: slope and bias of hyperplanes
	float *a = malloc(nH*dim*sizeof(float));
	float *b = malloc(nH*sizeof(float));

    float *XF = malloc(n*dim*sizeof(float));    for (i=0; i < n*dim; i++) { XF[i] = X[i]; }
    float *XWF = malloc(n*sizeof(float)); for (i=0; i < n; i++) { XWF[i] = XW[i]; }

	unzipParamsFloat(params,a,b,dim,nH,1);

	double *influence = malloc(nH*sizeof(double));
	double alpha = 1e-4, beta = 0.1;
	float gamma = 1;

	double *grad = malloc(nH*(dim+1)*sizeof(double));
	double *gradOld = malloc(nH*(dim+1)*sizeof(double));
	double *newtonStep = malloc(nH*(dim+1)*sizeof(double));
	double *paramsNew = malloc(lenP*sizeof(double));
	float *gradA = calloc(nH*(dim+1),sizeof(float));
	float *gradB = calloc(nH*(dim+1),sizeof(float));
	float *TermA = calloc(1,sizeof(float));
	float *TermB = calloc(1,sizeof(float));
	float TermAOld, TermBOld, funcVal, funcValStep;
	float lastStep;
	calcGradFloatAVXCaller(XF, XWF, gridFloat, a, b, gamma, weight, delta, n, dim, nH, lenY, gradA, gradB, TermA, TermB, influence, YIdx);
	sumGrad(grad,gradA,gradB,nH*(dim+1));
	
	copyVector(newtonStep,grad,nH*(dim+1),1);
	// LBFGS params
	int m = 40;
	double* s_k = calloc(lenP*m,sizeof(double));
	double* y_k = calloc(lenP*m,sizeof(double));
	double* sy = calloc(m,sizeof(double));
	double* syInv = calloc(m,sizeof(double));
	double lambdaSq, step;
	int iter, numIter;
	int activeCol = 0;
	// start the main iteration
	for (iter = 0; iter < 1e4; iter++) {
		lambdaSq = calcLambdaSq(grad,newtonStep,dim,nH);
		if (lambdaSq < 0 || lambdaSq > 1e5) {
			for (i=0; i < nH*(dim+1); i++) {
				newtonStep[i] = -grad[i];
			}
			lambdaSq = calcLambdaSq(grad,newtonStep,dim,nH);
		}

		step = 1;
		// objective function value before the step
		TermAOld = *TermA; TermBOld = *TermB; funcVal = TermAOld + TermBOld; copyVector(gradOld,grad,nH*(dim+1),0);
		// new parameters
		for (i=0; i < lenP; i++) { paramsNew[i] = params[i] + newtonStep[i]; }
		unzipParamsFloat(paramsNew,a,b,dim,nH,1);
		// calculate gradient and objective function value
		calcGradFloatAVXCaller(XF, XWF, gridFloat, a, b, gamma, weight, delta, n, dim, nH, lenY, gradA, gradB, TermA, TermB, influence, YIdx);
		sumGrad(grad,gradA,gradB,(dim+1)*nH);
		funcValStep = *TermA + *TermB;

		while (isnan(funcValStep) || isinf(funcValStep) || funcValStep > funcVal - step*alpha*lambdaSq) {
			if (step < 1e-9) {
				break;
			}
			step = beta*step;
			for (i=0; i < lenP; i++) {
				paramsNew[i] = params[i] + (newtonStep[i]*step);
			}
			unzipParamsFloat(paramsNew,a,b,dim,nH,1);

			calcGradFloatAVXCaller(XF, XWF, gridFloat, a, b, gamma, weight, delta, n, dim, nH, lenY, gradA, gradB, TermA, TermB, influence, YIdx);
			sumGrad(grad,gradA,gradB,(dim+1)*nH);
			funcValStep = *TermA + *TermB;
		}
		lastStep = funcVal - funcValStep;

		//Rprintf("%d: %.5f (%.4f, %.5f) \t (lambdaSq: %.4e, t: %.0e, Step: %.4e)\n",iter,funcValStep,-*TermA*n,*TermB,lambdaSq,step,lastStep);
		for (i=0; i < lenP; i++) { params[i] = paramsNew[i]; }
		
		if (fabs(1-*TermB) < intEps && lastStep < lambdaSqEps && iter > 10) {
			break;
		}
	
		// min([m,iter,length(b)]) --> C indexing of iter is one less than matlab --> +1
		numIter = m < iter+1 ? m : iter+1;
		numIter = lenP < numIter ? lenP : numIter;
		CNS(s_k,y_k,sy,syInv,step,grad,gradOld,newtonStep,numIter,activeCol,lenP,m);
		activeCol++; 
    	if (activeCol >= m) {
        	activeCol = 0;
		}
	}
	logLike[0] = funcValStep;
	free(gradA); free(gradB); free(TermA); free(TermB); free(a); free(b); free(delta); free(gridFloat); free(s_k); free(y_k); free(sy); free(syInv);
	free(grad); free(gradOld); free(newtonStep); free(paramsNew); free(XF); free(XWF);
}

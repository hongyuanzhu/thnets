#include <string.h>
#ifdef USEQSML
#include <stdbool.h>
typedef struct qsml_info qsml_info;
#include <qsml.h>
#endif

#include "../thnets.h"

#ifndef USEQSML
static void nn_unfolded_copy(THFloatTensor *finput, THFloatTensor *input,
	int kW, int kH, int dW, int dH, int padW, int padH,
	int nInputPlane, int inputWidth, int inputHeight,
	int outputWidth, int outputHeight)
{
	long k;
	float *input_data = THFloatTensor_data(input);
	float *finput_data = THFloatTensor_data(finput);

#pragma omp parallel for private(k)
	for(k = 0; k < nInputPlane*kH*kW; k++) {
		long nip = k / (kH*kW);
		long rest = k % (kH*kW);
		long kh = rest / kW;
		long kw = rest % kW;
		long x,y;
		long long ix,iy;
		float *dst = finput_data + nip*(kH*kW*outputHeight*outputWidth) + kh*(kW*outputHeight*outputWidth) + kw*(outputHeight*outputWidth);
		float *src = input_data + nip*(inputHeight*inputWidth);
		if (padW > 0 || padH > 0) {
			long lpad,rpad;
			for(y = 0; y < outputHeight; y++) {
				iy = (long long)(y*dH - padH + kh);
				if (iy < 0 || iy >= inputHeight) {
					memset(dst+y*outputWidth, 0, sizeof(float)*outputWidth);
				} else {
					if (dW==1){
						ix = (long long)(0 - padW + kw);
						lpad = thfmaxf(0,padW-kw);
						rpad = thfmaxf(0,padW-(kW-kw-1));
						if (outputWidth-rpad-lpad <= 0) {
							memset(dst+(y*outputWidth), 0, sizeof(float)*outputWidth);
						} else {
							if (lpad > 0) memset(dst+y*outputWidth, 0, sizeof(float)*lpad);
							memcpy(dst+(y*outputWidth+lpad), src+(iy*inputWidth+ix+lpad), sizeof(float)*(outputWidth-rpad-lpad));
							if (rpad > 0) memset(dst+y*outputWidth + outputWidth - rpad, 0, sizeof(float)*rpad);
						}
					}
					else{
						for (x=0; x<outputWidth; x++){
							ix = (long long)(x*dW - padW + kw);
							if (ix < 0 || ix >= inputWidth)
								memset(dst+(y*outputWidth+x), 0, sizeof(float)*1);
							else
								memcpy(dst+(y*outputWidth+x), src+(iy*inputWidth+ix), sizeof(float)*(1));
						}
					}
				}
			}
		} else {
			for(y = 0; y < outputHeight; y++) {
				iy = (long long)(y*dH + kh);
				ix = (long long)(0 + kw);
				if (dW == 1)
					memcpy(dst+(y*outputWidth), src+(iy*inputWidth+ix), sizeof(float)*outputWidth);
				else{
					for (x=0; x<outputWidth; x++)
						memcpy(dst+(y*outputWidth+x), src+(iy*inputWidth+ix+x*dW), sizeof(float)*(1));
				}
			}
		}
	}
}

static void nn_SpatialConvolutionMM_updateOutput_frame(THFloatTensor *input, THFloatTensor *output,
	THFloatTensor *weight, THFloatTensor *bias,
	THFloatTensor *finput,
	int kW, int kH, int dW, int dH, int padW, int padH,
	long nInputPlane, long inputWidth, long inputHeight,
	long nOutputPlane, long outputWidth, long outputHeight)
{
	THFloatTensor *output2d = 0;

	if(finput)
	{
		nn_unfolded_copy(finput, input, kW, kH, dW, dH, padW, padH,
			(int)nInputPlane, (int)inputWidth, (int)inputHeight, (int)outputWidth, (int)outputHeight);
		output2d = THFloatTensor_newWithStorage2d(output->storage, output->storageOffset,
			nOutputPlane, -1, outputHeight*outputWidth, -1);
	}

	long i;
	for (i = 0; i < nOutputPlane; i++)
	{
		float *data = output->storage->data + output->storageOffset + output->stride[0]*i;
		float what = bias && bias->storage ? THFloatTensor_data(bias)[i] : 0;
		long len = outputHeight*outputWidth;
		THFloatVector_fill(data, what, len);
	}

	if(finput)
	{
		THFloatTensor_addmm(output2d, 1, output2d, 1, weight, finput);
		THFloatTensor_free(output2d);
	}
#ifndef USEBLAS
	else THFloatTensor_convmm(output, 1, 1, weight, input, kH, kW, dH, dW, padH, padW);
#endif
}
#endif

#ifdef USEQSML
void applybias(struct module *newmod, int outP, int outW, int outH)
{
	int i, wsize;
	wsize = outW*outH;
	float* biasdata = THFloatTensor_data(newmod->SpatialConvolution.bias);
	float* outdata = THFloatTensor_data(newmod->output);
	//float* biasdata = (float*) calloc(wsize*outP, sizeof(float));//one memcpy is better?
	//memcpy(outdata, biasdata, wsize*outP*sizeof(float));//only saves 1ms
	for(i=0; i<wsize; i++)
		memcpy(outdata+i*outP, biasdata, outP*sizeof(float));
}
#endif

THFloatTensor *nn_SpatialConvolutionMM_updateOutput(struct module *module, THFloatTensor *input)
{
	int kW = module->SpatialConvolution.kW;
	int kH = module->SpatialConvolution.kH;
	int dW = module->SpatialConvolution.dW;
	int dH = module->SpatialConvolution.dH;
	int padW = module->SpatialConvolution.padW;
	int padH = module->SpatialConvolution.padH;

	THFloatTensor *finput = module->SpatialConvolution.finput;//thnets [col,row,plane,batch]
#ifndef USEQSML
	int batch = 1;
	THFloatTensor *bias   = module->SpatialConvolution.bias;
#endif
	THFloatTensor *output = module->output;//[3col,2row,1plane,0batch]
	THFloatTensor *weight = THFloatTensor_new();

	THFloatTensor_set(weight, module->SpatialConvolution.weight);
	THFloatTensor_resize2d(weight, weight->size[0], THFloatTensor_nElement(weight) / weight->size[0]);

	if (input->nDimension == 3)
	{
#ifndef USEQSML
		batch = 0;
#endif
		THFloatTensor_resize4d(input, 1, input->size[0], input->size[1], input->size[2]);
	}

	long batchSize = input->size[0];
	long nInputPlane  = module->SpatialConvolution.nInputPlane;
	long nOutputPlane = module->SpatialConvolution.nOutputPlane;
	long inputWidth   = input->size[3];
	long inputHeight  = input->size[2];
	long outputWidth  = (inputWidth + 2*padW - kW) / dW + 1;
	long outputHeight = (inputHeight + 2*padH - kH) / dH + 1;

	if(nInputPlane != input->size[1])
		THError("nInputPlane %ld does not match input planes %ld", nInputPlane, input->size[1]);

	if (outputWidth < 1 || outputHeight < 1)
		THError("Given input size: (%dx%dx%d). Calculated output size: (%dx%dx%d). Output size is too small",
		nInputPlane,inputHeight,inputWidth,nOutputPlane,outputHeight,outputWidth);

	if(module->type == MT_SpatialConvolutionMM)
		THFloatTensor_resize3d(finput, batchSize, kW*kH*nInputPlane, outputHeight*outputWidth);
	THFloatTensor_resize4d(output, batchSize, nOutputPlane, outputHeight, outputWidth);

#ifdef USEQSML

		qsml_int channel = nInputPlane;
		qsml_int inH = inputHeight;
		qsml_int inW = inputWidth;
		qsml_int qkW = kW;
		qsml_int qkH = kH;
		qsml_int qdW = dW;
		qsml_int qdH = dH;
		qsml_int qpadW = padW;
		qsml_int qpadH = padH;
		qsml_int outP = nOutputPlane;
		qsml_int outH = outputHeight;
		qsml_int outW = outputWidth;

		applybias(module,(int)outP,(int)outW,(int)outH);//doesn't add much overhead on average

		float *image = THFloatTensor_data(input);//[plane, col, row]
		float *filtro = THFloatTensor_data(weight);//[outplane, plane, col, row]
		float *outf = THFloatTensor_data(output);//[plane, col, row]

		sconv_mm(true, image, inW, inH, channel,
				 filtro, outP, qkW, qkH, qpadW, qpadH,
				 qdW, qdH, outf, outW, outH);

#else

	  long t;
#pragma omp parallel for if(batchSize >= 4) private(t)
	  for (t = 0; t < batchSize; t++) {
		  THFloatTensor *input_t = THFloatTensor_newSelect(input, 0, t);
		  THFloatTensor *output_t = THFloatTensor_newSelect(output, 0, t);
		  THFloatTensor *finput_t = module->type == MT_SpatialConvolutionMM ? THFloatTensor_newSelect(finput, 0, t) : 0;

		  nn_SpatialConvolutionMM_updateOutput_frame(input_t, output_t, weight, bias, finput_t,
			  kW, kH, dW, dH, padW, padH,
			  nInputPlane, inputWidth, inputHeight,
			  nOutputPlane, outputWidth, outputHeight);

		  THFloatTensor_free(input_t);
		  THFloatTensor_free(output_t);
		  THFloatTensor_free(finput_t);
	  }

	  if (batch == 0) {
		  THFloatTensor_resize3d(output, nOutputPlane, outputHeight, outputWidth);
		  THFloatTensor_resize3d(input, nInputPlane, inputHeight, inputWidth);
	  }
	  THFloatTensor_free(weight);
#endif

	return output;
}

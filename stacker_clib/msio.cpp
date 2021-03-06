// stacker, Python module for stacking of interferometric data.
// Copyright (C) 2014  Lukas Lindroos
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA. 
//
#include "msio.h"
#include "Chunk.h"
#include "definitions.h"
#include <iostream>
#include <stdlib.h>

#ifdef CASACORE_VERSION_2
#include <casacore/tables/Tables/TableError.h>
#include <casacore/tables/Tables/ExprNode.h>
#else
#include <tables/Tables/TableError.h>
#include <tables/Tables/ExprNode.h>
#endif

#ifdef CASACORE_VERSION_2
#include <casacore/ms/MSSel/MSSelection.h>
#endif
#ifdef LINKING_2_CASA_43
#include <ms/MSSel/MSSelection.h>
#endif
#ifndef LINKING_2_CASA_43
#ifndef CASACORE_VERSION_2
#include <ms/MeasurementSets/MSSelection.h>
#endif
#endif
using casa::TableError;
using std::cout;
using std::endl;
using casa::MSSelection;
using casa::TableExprNode;
using casa::Table;

msio::msio(const char* msinfile,
           const char * msoutfile,
		   int datacolumn,
		   const bool select_field, const char* field,
		   bool one_ptg_per_chunk) : DataIO()
{
#ifdef DEBUG
	cout << "Creating MeasurementSet object with msinfile = \"" << msinfile << "\"." << endl;
#endif
	msin = new MeasurementSet(msinfile);
	msin_nonsorted = NULL;
	if(select_field)
	{
#ifdef DEBUG
		cout << "Selecting field." << endl;
#endif
		MSSelection select;
		select.setFieldExpr(field);
		TableExprNode node = select.toTableExprNode(msin);
		Table tablesel(msin->tableName(), Table::Update);
		msin_nonsorted = msin;
		msin = new MeasurementSet(tablesel(node, node.nrow()));
	}
#ifdef DEBUG
	cout << "Creating ROMSColumns object." << endl;
#endif
	msincols = new ROMSColumns(*msin);
	one_ptg_per_chunk_ = one_ptg_per_chunk;
	ptg_warning_done = false;

	if(datacolumn == col_data)
	{
		datacolumn_ = col_data;
	}
	else if(datacolumn == col_model_data)
	{
		datacolumn_ = col_model_data;
		try
		{
			msin->isColumnStored("MODEL_DATA");
		}
		catch(TableError e)
		{
			throw fileException(fileException::MODEL_DATA_MISSING,
					"No \'model_data\' column exists in input mstable.");
		}
	}
	else
	{
#ifdef DEBUG
		cout << "Checking for CORRECTED_DATA column." << endl;
#endif
		datacolumn_ = col_corrected_data;
		try
		{
			msin->isColumnStored("CORRECTED_DATA");
		}
		catch(TableError e)
		{
			throw fileException(fileException::CORRECTED_DATA_MISSING,
					"No \'corrected_data\' column exists in input mstable.");
		}
	}

#ifdef DEBUG
	cout << "Open msoutfile if necessary." << endl;
#endif
	if(strlen(msoutfile) > 0)
	{
#ifdef DEBUG
		cout << "Creating msout as MeasurementSet object." << endl;
#endif
		msout = new MeasurementSet(msoutfile, casa::Table::Update);
		if(select_field)
		{
			MSSelection select;
			select.setFieldExpr(field);
			TableExprNode node = select.toTableExprNode(msout);
			Table tablesel(msout->tableName(), Table::Update);
			msout_nonsorted = msout;
			msout = new MeasurementSet(tablesel(node, node.nrow()));
		}
		msoutcols = new MSColumns(*msout);
		if(datacolumn_ == col_corrected_data) 
		{
			try{ msout->isColumnStored("CORRECTED_DATA");}
			catch(TableError e) 
			{
				throw fileException(fileException::CORRECTED_DATA_MISSING, 
						"No \'corrected_data\' column exists in output mstable.");
			}
		}
		else if(datacolumn_ == col_model_data)
		{
			try { msin->isColumnStored("MODEL_DATA"); }
			catch(TableError e)
			{
				throw fileException(fileException::MODEL_DATA_MISSING,
						"No \'model_data\' column exists in input mstable.");
			}
		}
	}
	else
	{
		msout = NULL;
		msoutcols = NULL;
	}
	currentVisibility = 0;

#ifdef DEBUG
	cout << "Find number of spectral windows and channels." << endl;
#endif
	nspw = (size_t)msincols->spectralWindow().nrow();
	nchan = 0;

	// First figure out the largest nchan value.
	for(size_t row =0 ; row < nspw; row++)
	{
		casa::Vector<double> freqbuff;
		msincols->spectralWindow().chanFreq().get(row, freqbuff);
		if((size_t)freqbuff.shape()(0) > nchan)
			nchan = (size_t)freqbuff.shape()(0);
	}

	cout << "nchan = " << nchan << endl;

	freq  = new float[nspw*nchan];

	// Now we can read the frequency information.
	for(size_t row =0 ; row < nspw; row++)
	{
		casa::Vector<double> freqbuff;
		msincols->spectralWindow().chanFreq().get(row, freqbuff);
		size_t c_nchan = (size_t) freqbuff.shape()(0);

		for(size_t col = 0; col < nchan; col++)
		{
			if(col < c_nchan) freq[row*nchan+col] = float(freqbuff(col));
			else              freq[row*nchan+col] = float(0.0);
		}
	}

	nstokes = 0;
	ROScalarColumn<casa::Int> nStokesCol = msincols->polarization().numCorr();
	Vector<casa::Int> nStokesForEach(msincols->polarization().nrow());
	nStokesCol.getColumn(nStokesForEach);
	for(casa::uInt i = 0; i < msincols->polarization().nrow(); i++)
	{
		if((size_t)nStokesForEach(i) > nstokes)
			nstokes = (size_t)nStokesForEach(i);
	}
	

	nfields = msin->field().nrow();
	x_phase_centre = new float[nfields];
	y_phase_centre = new float[nfields];
	for(int fieldID = 0; fieldID < nfields; fieldID++)
	{
		Array<double> phase_centre = msincols->field().phaseDir()(fieldID);
		x_phase_centre[fieldID] = float(phase_centre(IPosition(2,0,0)));
		y_phase_centre[fieldID] = float(phase_centre(IPosition(2,1,0)));

	}

}

msio::~msio()
{
	msin->flush();
	msin->closeSubTables();
	delete msincols;
	delete msin;
	if(msout)
	{
		delete msout;
		delete msoutcols;
	}
// 	for(int i = 0; i < nspw; i++)
// 		delete freq[i];
// 	delete freq;
}

size_t msio::nvis()
{
	return (size_t)msincols->data().nrow();
}

size_t msio::readChunk(Chunk& chunk)
{
// 	readChunkIteratorbased(chunk);
	return readChunkSimple(chunk);
}

size_t msio::readChunkDummy(Chunk& chunk)
{
	if(currentVisibility >= nvis()-1)
		return 0;
	currentVisibility += chunk.size();
	int uvrow = 0;
	for(size_t i = 0; i < chunk.size(); i++)
	{
		uvrow = i+currentVisibility;
		chunk.inVis[i].index = uvrow;
		chunk.inVis[i].u = float(0.);
		chunk.inVis[i].v = float(0.);
		chunk.inVis[i].w = float(0.);
		chunk.inVis[i].fieldID = 0;
		chunk.inVis[i].spw = 0;
		chunk.inVis[i].freq = &freq[nchan*chunk.inVis[i].spw];
	}
	return chunk.size();
}

size_t msio::readChunkIteratorbased(Chunk& chunk)
{
	return readChunkDummy(chunk);
}
// {/*{{{*/
// 	Vector<double> uvw;
// 	if(currentVisibility >= nvis()-1)
// 	{
// 		return 0;
// 	}
// 	else if(currentVisibility+chunk.size() > nvis())
// 		chunk.setSize(nvis()-currentVisibility);
// 
// 	int uvrow;
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		uvrow = i+currentVisibility;
// 		chunk.inVis[i].index = uvrow;
// 	}
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		chunk.inVis[i].spw = msincols->dataDescId()(chunk.inVis[i].index);
// 	}
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		chunk.inVis[i].data = datainit->matrix();
// 		datainit->next();
// 	}
// 
// // 	casa::MatrixIterator<Complex>* datait = (casa::MatrixIterator<Complex>*) msc.data().getColumn().makeIterator(2);
// // 		chunk.inVis[i].data = msincols.data()(uvrow);
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// // 		uvw = msincols.uvw()(uvrow);
// 		uvw = uvwinit->vector();
// 		chunk.inVis[i].u = float(uvw[0]);
// 		chunk.inVis[i].v = float(uvw[1]);
// 		chunk.inVis[i].w = float(uvw[2]);
// 		uvwinit->next();
// 	}
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		chunk.inVis[i].weight  = weightinit->vector();
// 		weightinit->next();
// 	}
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		chunk.inVis[i].freq = freq[chunk.inVis[i].spw];
// // 		chunk.inVis[i].freq  = new double[nchan];
// // 		for(int col = 0; col < nchan; col++)
// // 			chunk.inVis[i].freq[col] = freq[chunk.inVis[i].spw][col];
// 	}
// 	for(int i = 0; i < chunk.size(); i++)
// 	{
// 		uvrow = i+currentVisibility;
// 		chunk.inVis[i].fieldID = msincols->fieldId()(uvrow);
// 	}
// 	currentVisibility += chunk.size();
// 	return chunk.size();
// }/*}}}*/
//
size_t msio::readChunkSimple(Chunk& chunk)
{
	Vector<double> uvw;

	chunk.resetSize();
	chunk.set_dataset_id(dataset_id);

	size_t currentVisibility = this->currentVisibility;
	if(currentVisibility >= nvis())
		return 0;
	else if(currentVisibility+chunk.size() > nvis())
	{
		chunk.setSize(nvis()-currentVisibility);
	}


	if(one_ptg_per_chunk_)
	{
		int fieldID = msincols->fieldId()(currentVisibility);
		int inField = 0;
		while(inField < chunk.size() and msincols->fieldId()(inField+currentVisibility) == fieldID)
		{
			inField += 1;
		}
		if(inField < chunk.size())
		{
			ptg_breaks_in_a_row += 1;
			if(!ptg_warning_done and ptg_breaks_in_a_row > 1)
			{
				cout << "\nWarning! Few visibilities (" << inField 
				     << ") found in field " << fieldID << ". "
				     << "Code running on gpu may be inefficient if "
				     << "visibilties are not ordered after field. " << endl;
				ptg_warning_done = true;
			}
			chunk.setSize(inField);
		}
		else
		{
			ptg_breaks_in_a_row = 0;
		}
	}

	this->currentVisibility += chunk.size();

	chunk.reshape_data(this->nchan, this->nstokes);

	int uvrow = 0, nchan, nstokes;
	for(size_t i = 0; i < chunk.size(); i++)
	{
		Matrix<Complex> data;
		Matrix<bool> flag;
		Vector<float> weight;

		uvrow = i+currentVisibility;
		if(datacolumn_ == col_data)
		{
			msincols->data().get(uvrow, data);
		}
		else if(datacolumn_ == col_model_data)
		{
			msincols->modelData().get(uvrow, data);
		}
		else if(datacolumn_ == col_corrected_data)
		{
			msincols->correctedData().get(uvrow, data);
		}
		msincols->flag().get(uvrow, flag);
		msincols->weight().get(uvrow, weight);
		msincols->uvw().get(uvrow, uvw);

		chunk.inVis[i].index = uvrow;
		chunk.outVis[i].index = uvrow;

        nchan = data.ncolumn();
        nstokes = data.nrow();

		chunk.inVis[i].nchan = nchan;
		chunk.inVis[i].nstokes = nstokes;
		chunk.outVis[i].nchan = nchan;
		chunk.outVis[i].nstokes = nstokes;

        for(int stokes = 0; stokes < nstokes; stokes++)
        {
            chunk.inVis[i].weight[stokes] = float(weight(stokes));
            chunk.outVis[i].weight[stokes] = float(weight(stokes));
            for(int chan = 0; chan < nchan; chan++)
            {
                chunk.inVis[i].data_real[nchan*stokes+chan] = float(std::real(data(stokes,chan)));
                chunk.inVis[i].data_imag[nchan*stokes+chan] = float(std::imag(data(stokes,chan)));
                chunk.inVis[i].data_flag[nchan*stokes+chan] = int(flag(stokes,chan));
                chunk.outVis[i].data_flag[nchan*stokes+chan] = int(flag(stokes,chan));
            }
        }

		chunk.inVis[i].u = float(uvw[0]);
		chunk.inVis[i].v = float(uvw[1]);
		chunk.inVis[i].w = float(uvw[2]);
		chunk.inVis[i].fieldID = msincols->fieldId()(uvrow);
		chunk.outVis[i].fieldID = msincols->fieldId()(uvrow);

		chunk.inVis[i].spw  = msincols->dataDescId()(chunk.inVis[i].index);
		chunk.inVis[i].freq = &freq[nchan*chunk.inVis[i].spw];
		chunk.outVis[i].spw  = chunk.inVis[i].spw;
		chunk.outVis[i].freq  = chunk.inVis[i].freq;
	}
	return chunk.size();
}

void msio::writeChunk(Chunk& chunk)
{
	if(msout == NULL)
		return;

	Vector<double> uvw;
	for(size_t i = 0; i < chunk.size(); i++)
	{
		int nchan = chunk.outVis[i].nchan, 
			nstokes = chunk.outVis[i].nstokes;
		Matrix<Complex> data(nstokes, nchan);
		Matrix<bool> flag(nstokes, nchan);
		for(int chan = 0; chan < nchan; chan++)
		{
			for(int stokes = 0; stokes < nstokes; stokes++)
			{
				Complex vis = Complex(chunk.outVis[i].data_real[stokes*nchan+chan],
						              chunk.outVis[i].data_imag[stokes*nchan+chan]);
				data(stokes, chan) = vis;
				flag(stokes, chan) = chunk.outVis[i].data_flag[stokes*nchan+chan];
			}
		}
// 		msoutcols->data().put(chunk.outVis[i].index, data);
		if(datacolumn_ == col_data)
		{
			msoutcols->data().put(chunk.outVis[i].index, data);
		}
		else if(datacolumn_ == col_model_data)
		{
			msoutcols->modelData().put(chunk.outVis[i].index, data);
		}
		else if(datacolumn_ == col_corrected_data)
		{
			msoutcols->correctedData().put(chunk.outVis[i].index, data);
		}
		msoutcols->flag().put(chunk.outVis[i].index, flag);
	}
	for(size_t i = 0; i < chunk.size(); i++)
	{
		size_t nstokes = chunk.outVis[i].nstokes;
		Vector<Float> weight(nstokes);
		for(size_t stokes = 0; stokes<nstokes; stokes++)
			weight(stokes) = chunk.outVis[i].weight[stokes];

		msoutcols->weight().put(chunk.outVis[i].index, weight);
	}
	for(size_t i = 0; i < chunk.size(); i++)
	{
		msoutcols->fieldId().put(chunk.outVis[i].index, chunk.outVis[i].fieldID);
	}
}

int msio::nPointings()
{
	return nfields;
}

float msio::xPhaseCentre(int id)
{
	return x_phase_centre[id];
}

float msio::yPhaseCentre(int id)
{
	return y_phase_centre[id];
}

void msio::setPhaseCentre(int fieldID, double x, double y)
{
	casa::Array<double> newPhaseCentre(casa::IPosition(2,2,1));

	if(!msout)
		return;

	newPhaseCentre(IPosition(2,0,0)) = x;
	newPhaseCentre(IPosition(2,1,0)) = y;

	msoutcols->field().phaseDir().put(fieldID, newPhaseCentre);
	msoutcols->field().referenceDir().put(fieldID, newPhaseCentre);
	msoutcols->field().delayDir().put(fieldID, newPhaseCentre);
}

size_t msio::nStokes()
{
	return nstokes;
}

size_t msio::nChan()
{
	return nchan;
}

size_t msio::nSpw()
{
	return nspw;
}

float* msio::getFreq(int spw)
{
	return &freq[spw*nchan];
}

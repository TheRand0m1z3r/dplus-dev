#include <set>
#include "Eigen/Core"
#include <sstream>
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

#include "boost/filesystem.hpp"
#include "boost/filesystem/fstream.hpp"
#include <boost/algorithm/string/predicate.hpp>

#include <iostream>
#include <regex>

namespace fs = boost::filesystem;

#include "PDBReaderLib.h"
#include <numeric>

namespace PDBReader {

	using std::make_pair;

	// In order for exporting to work, explicitly instantiate the class for the 
	// float and double types (and more as necessary)
#ifdef _WIN32
	template class PDBReaderOb<float>;
	template class PDBReaderOb<double>;
#else
	// For some reason with g++ templates should be instantiated like so
	template PDBReaderOb<float> ::PDBReaderOb();
	template PDBReaderOb<float> ::PDBReaderOb(string filename, bool moveToCOM, int model, std::string anomalousFName);
	template PDBReaderOb<double> ::PDBReaderOb();
	template PDBReaderOb<double> ::PDBReaderOb(string filename, bool moveToCOM, int model, std::string anomalousFName);
#endif

	template<class FLOAT_TYPE>
	PDBReaderOb<FLOAT_TYPE>::PDBReaderOb(string filename, bool moveToCOM, int model /*= 0*/, string anomalousFName /*= ""*/)
	{
		status = UNINITIALIZED;

		initialize();

		fn = filename;
		anomalousfn = anomalousFName;

		bMoveToCOM = moveToCOM;

		if (anomalousfn.size())
			status = readAnomalousfile(anomalousFName);

		if (status == UNINITIALIZED || status == PDB_OK)
			status = readPDBfile(filename, moveToCOM, model);
	}

	template<class FLOAT_TYPE>
	PDBReaderOb<FLOAT_TYPE>::PDBReaderOb()
	{
		status = UNINITIALIZED;
		initialize();
	}

	template<class FLOAT_TYPE>
	PDBReaderOb<FLOAT_TYPE>::~PDBReaderOb()
	{
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::moveCOMToOrigin()
	{
		FLOAT_TYPE xx = 0.0, yy = 0.0, zz = 0.0, wt = 0.0, aWt = 0.0;
		for (size_t i = 0; i < x.size(); i++)
		{
			aWt = atmWt[atmInd[i]];
			xx += x[i] * aWt;
			yy += y[i] * aWt;
			zz += z[i] * aWt;
			wt += aWt;
		}

		xx /= wt;
		yy /= wt;
		zz /= wt;

		for (size_t i = 0; i < x.size(); i++)
		{
			x[i] -= xx;
			y[i] -= yy;
			z[i] -= zz;
		}
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::moveGeometricCenterToOrigin()
	{
		Eigen::Map<Eigen::Array<FLOAT_TYPE, -1, 1>> xm(x.data(), x.size(), 1);
		Eigen::Map<Eigen::Array<FLOAT_TYPE, -1, 1>> ym(y.data(), y.size(), 1);
		Eigen::Map<Eigen::Array<FLOAT_TYPE, -1, 1>> zm(z.data(), z.size(), 1);

		xm -= xm.mean();
		ym -= ym.mean();
		zm -= zm.mean();
	}

	template<class FLOAT_TYPE>
	Eigen::Matrix<FLOAT_TYPE, 3, 3, 0, 3, 3> EulerD(Radian theta, Radian phi, Radian psi)
	{
		FLOAT_TYPE ax, ay, az, c1, c2, c3, s1, s2, s3;
		ax = theta;
		ay = phi;
		az = psi;
		c1 = cos(ax); s1 = sin(ax);
		c2 = cos(ay); s2 = sin(ay);
		c3 = cos(az); s3 = sin(az);
		Eigen::Matrix<FLOAT_TYPE, 3, 3, 0, 3, 3> rot;
		rot << c2*c3, -c2*s3, s2,
			c1*s3 + c3*s1*s2, c1*c3 - s1*s2*s3, -c2*s1,
			s1*s3 - c1*c3*s2, c3*s1 + c1*s2*s3, c1*c2;
		return rot;
	}

	/************************
	Rotate to primary axis
	and rewrite the PDB file
	************************/
	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::AlignPDB()
	{
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4244)
#endif
		std::string filename = fn;
		// CoM
		FLOAT_TYPE xx = 0.0, yy = 0.0, zz = 0.0, wt = 0.0, aWt = 0.0;
		for (size_t i = 0; i < x.size(); i++)
		{
			aWt = atmWt[atmInd[i]];
			xx += x[i] * aWt;
			yy += y[i] * aWt;
			zz += z[i] * aWt;
			wt += aWt;
		}
		for (size_t i = 0; i < x.size(); i++)
		{
			x[i] -= xx / wt;
			y[i] -= yy / wt;
			z[i] -= zz / wt;
		}

		Eigen::Matrix<FLOAT_TYPE, -1, -1, 0, -1, -1> coo, ee, finM;
		coo.resize(x.size(), 3);
		for (size_t i = 0; i < x.size(); i++)
			coo.row(i) << x[i], y[i], z[i];
		ee = coo.transpose() * coo;
		typedef Eigen::Matrix<FLOAT_TYPE, -1, -1, 0, -1, -1> fmat;
		Eigen::SelfAdjointEigenSolver<fmat> es(ee);
		FLOAT_TYPE aa, bb, cc;
		aa = es.eigenvalues()(0);
		bb = es.eigenvalues()(1);
		cc = es.eigenvalues()(2);
		int cInd = 0;
		// Easy because they're ordered
		if (fabs(bb - aa) > fabs(cc - bb))
			cInd = 0;
		else
			cInd = 2;
		Eigen::Matrix<FLOAT_TYPE, 3, 1, 0, 3, 1> Z, Y, rotAxis, nz = es.eigenvectors().col(cInd);
		Z << 0.0, 0.0, 1.0;
		Y << 0.0, 1.0, 0.0;
		rotAxis = Z.cross(nz);
		rotAxis = rotAxis / rotAxis.norm();
		FLOAT_TYPE cs = Z.dot(nz) / (nz.norm());
		if (fabs(cs) > 1.0) cs = cs > 0. ? 1. : -1.;	// Correct if (z/r) is greater than 1.0 by a few bits
		FLOAT_TYPE
			thet = acos(-cs),
			sn = sin(thet);
		FLOAT_TYPE ux = rotAxis(0), uy = rotAxis(1), uz = rotAxis(2);

		Eigen::Matrix<FLOAT_TYPE, 3, 3, 0, 3, 3> rot;
		rot << cs + sq(ux)*(1.0 - cs), ux*uy*(1.0 - cs) - uz*sn, ux*uz*(1.0 - cs) + uy*sn,
			uy*ux*(1.0 - cs) + uz*sn, cs + sq(uy)*(1.0 - cs), uy*uz*(1.0 - cs) - ux*sn,
			uz*ux*(1.0 - cs) - uy*sn, uz*uy*(1.0 - cs) + ux*sn, cs + sq(uz)*(1.0 - cs);
		// Rotate the PDB coordinates
		ee = (rot.transpose() * coo.transpose()).transpose();
		// Second axis
		Eigen::SelfAdjointEigenSolver<fmat> es2(ee.transpose() * ee);
		nz = es2.eigenvectors().col(1);
		rotAxis = Y.cross(nz);
		rotAxis = rotAxis / rotAxis.norm();
		ux = rotAxis(0); uy = rotAxis(1); uz = rotAxis(2);
		cs = Y.dot(nz) / nz.norm();
		if (fabs(cs) > 1.) cs = cs > 0. ? 1. : -1.;
		thet = acos(-cs);
		sn = sin(thet);
		rot << cs + sq(ux)*(1.0 - cs), ux*uy*(1.0 - cs) - uz*sn, ux*uz*(1.0 - cs) + uy*sn,
			uy*ux*(1.0 - cs) + uz*sn, cs + sq(uy)*(1.0 - cs), uy*uz*(1.0 - cs) - ux*sn,
			uz*ux*(1.0 - cs) - uy*sn, uz*uy*(1.0 - cs) + ux*sn, cs + sq(uz)*(1.0 - cs);

		finM = 10.0 * (rot.transpose() * ee.transpose()).transpose();
		std::ifstream inFile(filename.c_str());
		fs::path a1, a2, a3;
		fs::path pathName(filename), nfnm;
		pathName = fs::system_complete(filename);

		a1 = pathName.parent_path();
		a2 = fs::path(pathName.stem().string() + "_rot");
		a3 = pathName.extension();
		nfnm = (a1 / a2).replace_extension(a3);
		std::ofstream outFile(nfnm.string().c_str());
		string line;
		string name;//, atom;
		int pp = 0;
		while (getline(inFile, line)) {
			name = line.substr(0, 6);
			if (name == "ATOM  ") {
				string lnn, xSt, ySt, zSt;
				lnn.resize(line.length());
				xSt.resize(24);
				ySt.resize(24);
				zSt.resize(24);
				sprintf(&xSt[0], "%8f", finM(pp, 0));
				sprintf(&ySt[0], "%8f", finM(pp, 1));
				sprintf(&zSt[0], "%8f", finM(pp, 2));
				sprintf(&lnn[0], "%s%s%s%s%s", line.substr(0, 30).c_str(), xSt.substr(0, 8).c_str(),
					ySt.substr(0, 8).c_str(), zSt.substr(0, 8).c_str(), line.substr(54, line.length() - 1).c_str());
				pp++;
				outFile << lnn << "\n";
			}
			else {
				outFile << line << "\n";
			}
		}
		inFile.close();
		outFile.close();
#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif
	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readAnomalousbuffer(const char *buffer, size_t buffSize)
	{
		std::istringstream inBuffer(buffer);

		if (!buffer || buffSize == 0) {
			std::cout << "Error opening buffer for reading.\n";
			return NO_FILE; //TODO: FIX
		}

		PDB_READER_ERRS err = PDB_OK;
		try
		{
			err = readAnomalousstream(inBuffer);
		}
		catch (pdbReader_exception &e)
		{
			std::cout << "Error in anomalous file: " << e.what() << "\n\n";
			return e.GetErrorCode();
		}

		return err;

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readAnomalousfile(string filename)
	{
		std::ifstream inFile(filename.c_str());

		if (!inFile) {
			std::cout << "Error opening file " << filename << " for reading.\n";
			return NO_FILE; //TODO: FIX
		}
		
		PDB_READER_ERRS err = PDB_OK;
		try
		{
			err = readAnomalousstream(inFile);
		}
		catch (pdbReader_exception &e)
		{
			std::cout << "Error in anomalous file: " << e.what() << "\n\n";
			return e.GetErrorCode();
		}
		inFile.close();

		return err;

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readAnomalousstream(std::istream& inFile)
	{
		string line;

		const std::string RX_EOL("[^.]*$");	// Weird in order to hand cross-platform line endings
		const std::string RX_FLOATING_POINT_STRING("[-\\+]?\\b\\d*\\.?\\d+(?:[eE][-\\+]?\\d+)?");
		const std::string RX_LINE_START("^\\s*");
		const std::string RX_COMMENT("\\s*#\\s*");
		const std::string RX_EVERYTHING(".*");
		const std::string RX_WHITESPACE("\\s*");
		const std::string RX_INTEGER("\\b\\d+\\b");
		const std::string RX_BEGIN_REFERENCE("\\s*(");
		const std::string RX_END_REFERENCE(")");
		const std::string RX_ION_TYPE("[A-Z][a-z]?(?:\\d[-\\+]|[-\\+]\\d)?");
		const std::string RX_GROUP_NAME("\\$\\([^\\)]+\\)");
		const std::string RX_GROUP_OF_INTEGERS("\\s*(?:" + RX_INTEGER + "\\s*[,;]?\\s*)*" + RX_INTEGER + "\\s*");

		const auto regexType = std::regex::ECMAScript | std::regex::icase;

		std::regex rx_BlankLine;
		std::regex rx_Energy;
		std::regex rx_ionType;
		std::regex rx_plainGroup;
		std::regex rx_referenceToGroupBelow;
		std::regex rx_GroupBelow;
		std::regex rx_Comment;

		const std::string RX_FPRIMES(
			RX_BEGIN_REFERENCE + RX_FLOATING_POINT_STRING + RX_END_REFERENCE +
			"\\s+" + RX_BEGIN_REFERENCE + RX_FLOATING_POINT_STRING + RX_END_REFERENCE + "\\s+");
		try {

			rx_BlankLine.assign(RX_LINE_START + RX_WHITESPACE + RX_EOL, regexType);

			rx_Energy.assign(RX_LINE_START + RX_COMMENT + "Energy[[:space:]]+" +
				RX_BEGIN_REFERENCE + RX_EVERYTHING + RX_END_REFERENCE + RX_EOL
				, regexType);

			rx_ionType.assign(
				RX_LINE_START + RX_FPRIMES +
				RX_BEGIN_REFERENCE + RX_ION_TYPE + RX_END_REFERENCE +
				RX_WHITESPACE + RX_EOL
				, regexType);

			rx_plainGroup.assign(
				RX_LINE_START + RX_FPRIMES +
				RX_BEGIN_REFERENCE + RX_GROUP_OF_INTEGERS + RX_END_REFERENCE + RX_WHITESPACE + RX_EOL
				, regexType);

			rx_referenceToGroupBelow.assign(
				RX_LINE_START + RX_FPRIMES +
				RX_BEGIN_REFERENCE + RX_GROUP_NAME + RX_END_REFERENCE + RX_WHITESPACE + RX_EOL
				, regexType);

			rx_GroupBelow.assign(
				RX_LINE_START + RX_BEGIN_REFERENCE + RX_GROUP_NAME + RX_END_REFERENCE +
				RX_BEGIN_REFERENCE + RX_GROUP_OF_INTEGERS + RX_END_REFERENCE + RX_WHITESPACE + RX_EOL
				, regexType);

			rx_Comment.assign(RX_LINE_START + RX_COMMENT //+ RX_WHITESPACE + RX_EOL
				, regexType);
		}
		catch (std::regex_error& e) {
			std::cout << "Caught regex exception:\n\n" << e.what() << "\n\n";
		}

		std::smatch match, secondaryMatches;
		int matches = 0;

		std::map<string, std::set<int>> groupNameToIndices;
		std::map<string, fpair> groupNameToFPrimes;

		try	{
			while (getline(inFile, line)) {
				matches = 0;

				if (std::regex_search(line, match, rx_Comment))
				{
					matches++;
					continue;
				}

				if (std::regex_search(line, match, rx_Energy))
				{
					matches++;
					std::string energyString = match.str(1);
					if (std::regex_search(energyString, secondaryMatches, std::regex("(" + RX_FLOATING_POINT_STRING + ".*[^\\s])\\s*$", regexType)))
					{
						energyStr = secondaryMatches.str(1);
					}

				}

				if (std::regex_search(line, match, rx_ionType))
				{
					if (matches) throw pdbReader_exception(FILE_ERROR, "You broke my regex abilities.");
					string atomType = match.str(3);

					// Normalize the charge order
					atomType = std::regex_replace(atomType, std::regex("(\\d)([-\\+])"), std::string("$2$1"));

					switch (atomType.size())
					{
					case 1:
						atomType.resize(4, ' ');
						atomType[1] = atomType[0];
						atomType[0] = ' ';
						break;
					case 2:
						atomType.resize(4, ' ');
						break;
					case 3:
						atomType.resize(4, ' ');
						std::rotate(atomType.rbegin(), atomType.rbegin() + 1, atomType.rend());
						break;
					case 4:
						break;

					default:
						throw pdbReader_exception(FILE_ERROR, "You broke my regex abilities.");
					}

					// Normalize the letters
					atomType[0] = std::tolower(atomType[0], std::locale::classic());
					atomType[1] = std::tolower(atomType[1], std::locale::classic());

					// Determine if the atom type was already read				
					auto it = anomTypes.find(atomType);
					if (it != anomTypes.end())
						throw pdbReader_exception(FILE_ERROR, "An anomalous atom type cannot be input twice.");
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4244)
#endif
					anomTypes[atomType] = fpair(stod(match.str(1)), stod(match.str(2)));
#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif

					matches++;
				}

				if (std::regex_search(line, match, rx_plainGroup))
				{
					if (matches) throw pdbReader_exception(FILE_ERROR, "You broke my regex abilities.");

					auto fprimes = fpair(stod(match.str(1)), stod(match.str(2)));

					auto it = anomGroups.find(fprimes);

					if (it == anomGroups.end())
					{
						anomGroups[fprimes] = std::vector<int>();
						it = anomGroups.find(fprimes);
					}

					string integersAsStrings = match.str(3);

					std::regex tmprx("\\b\\d+\\b");
					for (std::sregex_token_iterator itt(integersAsStrings.begin(), integersAsStrings.end(), tmprx);
						itt != std::sregex_token_iterator(); ++itt)
						it->second.push_back(stoi(itt->str()));

					matches++;
				}

				if (std::regex_search(line, match, rx_referenceToGroupBelow))
				{
					if (matches) throw pdbReader_exception(FILE_ERROR, "You broke my regex abilities.");

					string gname = match.str(3);

					// Normalize the letters ?
					//std::transform(gname.begin(), gname.end(), gname.begin(), ::tolower);

					// Determine if the groupname type was already read				
					auto it = groupNameToFPrimes.find(gname);
					if (it != groupNameToFPrimes.end())
						throw pdbReader_exception(FILE_ERROR, "An anomalous atom group cannot be input twice.");

					groupNameToFPrimes[gname] = fpair(stod(match.str(1)), stod(match.str(2)));

					matches++;
				}

				if (std::regex_search(line, match, rx_GroupBelow))
				{
					if (matches) throw pdbReader_exception(FILE_ERROR, "You broke my regex abilities.");

					string gname = match.str(1);

					// Normalize the letters ?
					//std::transform(gname.begin(), gname.end(), gname.begin(), ::tolower);

					// Determine if the groupname type was already read
					auto it = groupNameToIndices.find(gname);
					if (it == groupNameToIndices.end())
					{
						groupNameToIndices.insert(make_pair(gname, std::set<int>()));
						it = groupNameToIndices.find(gname);
					}

					string integersAsStrings = match.str(2);

					const std::sregex_token_iterator theEnd;
					std::regex tmprx("\\b\\d+\\b");
					for (std::sregex_token_iterator itt(integersAsStrings.begin(), integersAsStrings.end(), tmprx);
						itt != theEnd; ++itt)
						it->second.insert(stoi(itt->str()));

					matches++;
				}

				if (matches == 1)
					continue;


				if (std::regex_search(line, match, rx_BlankLine))
					continue;


				throw pdbReader_exception(FILE_ERROR, std::string("Invalid line in anomalous file:\n" + line).c_str());

			} // while

			// Merge the named groups into plain groups
			for (auto& kv : groupNameToFPrimes)
			{
				auto found = groupNameToIndices.find(kv.first);
				if (found == groupNameToIndices.end())
					throw pdbReader_exception(FILE_ERROR, "One of the used groups does not have a corresponding definition");

				auto it = anomGroups.find(fpair(kv.second.first, kv.second.second));
				if (it == anomGroups.end())
				{
					anomGroups.insert(make_pair(fpair(kv.second.first, kv.second.second), vector<int>()));
					it = anomGroups.find(fpair(kv.second.first, kv.second.second));
				}

				it->second.insert(it->second.end(), found->second.begin(), found->second.end());
			}

			// Make sure that there are no duplicates between groups. Eww.
			for (auto it = anomGroups.begin(); it != anomGroups.end(); ++it)
			{
				auto& outerVec = it->second;

				auto innerIt = it;
				// Skip the first one, as we don't want to compare something to itself
				for (++innerIt; innerIt != anomGroups.end(); ++innerIt)
				{
					auto& innerVec = innerIt->second;

					for (auto ov : outerVec)
					{
						for (auto iv : innerVec)
						{
							if (ov == iv)
								throw pdbReader_exception(FILE_ERROR, "A single atom cannot be listed in two different groups.");
						}
					}
				}
			} // for it

		} // try
		catch (pdbReader_exception& e)
		{
			std::cout << e.GetErrorMessage() << "\n";
			return e.GetErrorCode();
		}

		haveAnomalousAtoms = (anomGroups.size() + anomTypes.size() > 0);

		// Reverse the map of the groups so that the lookup based on index is quick
		for (auto& k : anomGroups)
		{
			for (auto i : k.second)
			{
				anomIndex.insert(make_pair(i, k.first));
			}
		}

		return PDB_OK;

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readPDBbuffer(const char *buffer, size_t buffSize, bool bCenter, int model /*= 0*/) {
		std::istringstream inBuffer(buffer);


		if (!buffer || buffSize == 0) {
			std::cout << "Error opening buffer for reading.\n";
			status = NO_FILE;
		}

		bMoveToCOM = bCenter;

		PDB_READER_ERRS err = readPDBstream(inBuffer, bCenter, model);

		return err;
	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readPDBfile(string filename, bool bCenter, int model) {
		std::ifstream inFile(filename.c_str());

		if (!inFile) {
			std::cout << "Error opening file " << filename << " for reading.\n";
			return NO_FILE;
		}

		bMoveToCOM = bCenter;

		PDB_READER_ERRS err = readPDBstream(inFile, bCenter, model);
		inFile.close();

		return err;
	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readPDBstream(std::istream& inFile, bool bCenter, int model) {
		string line;
		string name, last4chars;//, atom;
		string serialNumber, atomName, resName, resNo, segId;
		FLOAT_TYPE xx, yy, zz, occ, b;
		bool ignoreModel = false;
		bMoveToCOM = bCenter;
		number_of_implicit_amino_acids = number_of_implicit_atoms = 0;

		std::regex isDigitsOnly("^\\s*\\d+\\s*$");

		try	{
			while (getline(inFile, line)) {
				name = line.substr(0, 6);
				if (name == "ATOM  " || name == "HETATM") {	// Most common first
					if (ignoreModel)
						continue;
					if (line.substr(16, 2)[0] != ' ' && line.substr(16, 2)[0] != 'A')	// Alternate location
						continue;
					// Check to make sure that it's has enough chars (PDB < v2.0 had only 70 chars)
					last4chars = (line.length() > 76 ? line.substr(76, 4) : "");	// Includes charge
					if (last4chars.find_first_not_of(" \t\n\v\f\r") == std::string::npos) // If it's all whitespace, don't use it
						last4chars.clear();
					switch (last4chars.length()) {	// The charge was not included
					case 4:
						if (last4chars[3] == 13)
							last4chars[3] = ' ';

					case 3:
						if (last4chars[2] == 13)	// Carriage return
							last4chars.erase(2);
						else
							break;
					case 2:		// Assume the atom is neutral.
						last4chars.append("  ");
						break;
					case 0:
						// Try to identify atom based on characters 13-16
						last4chars.append(line.substr(12, 2));
						last4chars.append("  ");
						if (last4chars[0] >= '0' && last4chars[0] <= '9')
							last4chars[0] = ' ';
					case 1:
						break;
					}


					serialNumber = line.substr(6, 5);
					atomName = line.substr(12, 4);
					resName = line.substr(17, 3);
					resNo = line.substr(22, 4);
// Templated code, produces conversion warnings
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4244)
#endif
					xx = atof(string(line.substr(30, 8)).c_str()) / 10.0;
					yy = atof(string(line.substr(38, 8)).c_str()) / 10.0;
					zz = atof(string(line.substr(46, 8)).c_str()) / 10.0;
					occ = atof(string(line.substr(54, 6)).c_str());
					b = atof(string(line.substr(60, 6)).c_str());
#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif
					segId = (line.length() > 72 ? line.substr(72, 4) : "");

					//////////////////////////////////////////////////////////////////////////
					// Insert anomalous check here as an easy method of separating the
					//      normal and anomalous atoms

					if (haveAnomalousAtoms)
					{
						std::regex rx_tmp("(\\d)([-\\+])");
						string atomNorm = std::regex_replace(last4chars, rx_tmp, std::string("$2$1"));
						// Normalize the letters
						atomNorm[0] = std::tolower(last4chars[0], std::locale::classic());
						atomNorm[1] = std::tolower(last4chars[1], std::locale::classic());

						auto itType = anomTypes.find(atomNorm);
						bool foundByType = itType != anomTypes.end();
						int sn = -1;
						if (std::regex_match(serialNumber, isDigitsOnly))
							sn = atoi(serialNumber.c_str());
						auto itInd = anomIndex.find(sn);
						bool foundByIndex = itInd != anomIndex.end();

						if (foundByIndex && foundByType)
							throw std::runtime_error("Atom belongs to two groups with different anomalous scattering parameters.");

						typedef std::complex<typename decltype(anomfPrimes)::value_type::value_type> complex_type;

						complex_type fprimes(0, 0);
						if (foundByIndex | foundByType)
						{
							if (foundByType)
								fprimes = complex_type(itType->second.first, itType->second.second);
							else
								fprimes = complex_type(itInd->second.first, itInd->second.second);
						}
						anomfPrimes.push_back(fprimes);
					}

					pdbAtomSerNo.push_back(serialNumber);
					pdbAtomName.push_back(atomName);
					pdbResName.push_back(resName);
					pdbChain.push_back(line[21]);
					pdbResNo.push_back(resNo);

					x.push_back(xx);
					y.push_back(yy);
					z.push_back(zz);
					occupancy.push_back(occ);
					BFactor.push_back(b);
					pdbSegID.push_back(segId);

					atom.push_back(last4chars);

					continue;
				}
				if (name == "HEADER") {	// First line of the PDB file
					continue;
				}
				if (name == "TITLE ") {
					continue;
				}
				if (name == "CRYST1") {
					continue;
				}
				if (name == "ORIGX1" || name == "ORIGX2" || name == "ORIGX3") {
					continue;
				}
				if (name == "SCALE1" || name == "SCALE2" || name == "SCALE3") {
					continue;
				}
				if (name == "MTRIX1" || name == "MTRIX2" || name == "MTRIX3") {
					continue;
				}
				if (name == "TVECT ") {
					continue;
				}
				if (name == "MODEL ") {
					if (model > 0 && atoi(string(line.substr(10, 4)).c_str()) != model)
						ignoreModel = true;

					continue;
				}
				if (name == "ANISOU") {
					continue;
				}
				if (name == "TER   ") {
					continue;
				}
				if (name == "ENDMDL") {
					ignoreModel = false;
					continue;
				}
				// If we've reached this point, we are going to ignore the line
				continue;
			} // while
		} // try
		catch (std::exception& e)
		{
			std::cout << e.what() << "\n";
			return ERROR_IN_PDB_FILE;
		}

		/*
		catch(...) {
		return ERROR_IN_PDB_FILE;
		}
		*/
		if (x.size() == 0) // Didn't I do this already somewhere?
			throw pdbReader_exception(NO_ATOMS_IN_FILE, "The file does not contains any valid ATOM or HETATM lines.");
		
		//////////////////////////////////////////////////////////////////////////
		// This section changes the name of some atoms to a group name iff there
		// are no hydrogens in the amino acid and it's an amino acid.
		{
			implicitAtom.resize(atom.size(), "");
			int i = 0;
			while (i < atom.size())
			{
				std::string current_residue_index = pdbResNo[i];
				std::string amino_acid_name = pdbResName[i];
				bool residue_has_hydrogen = false;
				int residue_size = atom.size() - i;
				// Get size of amino acid/residue and check if there are any hydrogen atoms
				for (int j = i; j < atom.size(); j++)
				{
					if (pdbResNo[j] != current_residue_index || amino_acid_name != pdbResName[j])
					{
						residue_size = j - i;
						break;
					}
					if (atom[j] == " H  " || atom[j] == " h  ")
					{
						residue_has_hydrogen = true;
					}
				} // for j

				if (!residue_has_hydrogen)
				{
					ChangeResidueGroupsToImplicit(amino_acid_name, i, residue_size);
				} // if (!aa_has_hydrogen)

				i += residue_size;
			} // while i
		} // block

		std::cout << number_of_implicit_atoms << " of " << atom.size() <<
			" atoms are treated as having implicit hydrogen atoms (" << number_of_implicit_amino_acids << " amino acids).\n";

		atmInd.resize(x.size(), -1);
		ionInd.resize(x.size(), -1);
#pragma omp parallel for
		for (size_t i = 0; i < atmInd.size(); i++) {
			string atm = implicitAtom[i].length() > 0 ? implicitAtom[i] : (string(atom[i]).substr(0, 4));
			getAtomIonIndices(atm, atmInd[i], ionInd[i]);
			// atmInd[i]--;	// There is a dummy at wt[0]
			ionInd[i]--;	// No dummy in the coefs matrix
		} // for

		if (this->bMoveToCOM) {
			moveCOMToOrigin();
		}

		// Sort for speed in calculation
		/**/
		size_t vecSize = x.size();
		std::vector< IonEntry<FLOAT_TYPE> > entries(vecSize);

		for (size_t i = 0; i < vecSize; i++) {
			entries[i].BFactor = BFactor[i];
			entries[i].ionInd = ionInd[i];
			entries[i].atmInd = atmInd[i];
			entries[i].x = x[i];
			entries[i].y = y[i];
			entries[i].z = z[i];
			entries[i].fPrime = (anomfPrimes.size() > 0) ? anomfPrimes[i].real() : 0.f;
			entries[i].fPrimePrime = (anomfPrimes.size() > 0) ? anomfPrimes[i].imag() : 0.f;
		}

		std::sort(entries.begin(), entries.end(), SortIonEntry<FLOAT_TYPE>);
		sortedAtmInd.resize(vecSize);
		sortedIonInd.resize(vecSize);
		sortedX.resize(vecSize);
		sortedY.resize(vecSize);
		sortedZ.resize(vecSize);
		sortedBFactor.resize(vecSize);
		atomLocs.resize(vecSize);
		sortedCoeffIonInd.resize(vecSize);
		atomsPerIon.resize(vecSize);
		sortedAnomfPrimes.resize(vecSize);

		ExtractRelevantCoeffs(entries, vecSize, sortedIonInd, sortedAtmInd, atomsPerIon,
			sortedCoeffIonInd, sortedX, sortedY, sortedZ, atomLocs, sortedBFactor, sortedCoeffs, &sortedAnomfPrimes);

		return PDB_OK;

	} //readPDBfile

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::initialize() {

		this->haveAnomalousAtoms = false;
		this->bOutputSlices = false;
		this->atmRadType = RAD_UNINITIALIZED;
		this->bOnlySolvent = false;

		const int number_of_atoms_and_groups =
			119 + // Basic atoms
			8;	// CH, CH2, CH3, NH, NH2, NH3, OH, SH

		atmWt.resize(number_of_atoms_and_groups, 0.0);
		vdwRad.resize(number_of_atoms_and_groups, 0.0);
		empRad.resize(number_of_atoms_and_groups, 0.0);
		calcRad.resize(number_of_atoms_and_groups, 0.0);
		svgRad.resize(number_of_atoms_and_groups, 0.0);
#pragma region Atomic weights and radii
// Templated code, assignments produce conversion/truncation warnings
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4305)
#pragma warning( disable : 4244)
#endif

		atmWt[0] = 0.0;		vdwRad[0] = 0.0;	empRad[0] = 0.0;	calcRad[0] = 0.0;
		atmWt[1] = 1.008;	vdwRad[1] = 120.0;	empRad[1] = 25.0;	calcRad[1] = 53.0;	//	H	hydrogen
		atmWt[2] = 4.002602;	vdwRad[2] = 140.0;	empRad[2] = 31.0;	calcRad[2] = 31.0;	//	He	helium
		atmWt[3] = 6.94;		vdwRad[3] = 182.0;	empRad[3] = 145.0;	calcRad[3] = 167.0;	//	Li	lithium
		atmWt[4] = 9.012182;	vdwRad[4] = 153.0;	empRad[4] = 105.0;	calcRad[4] = 112.0;	//	Be	beryllium
		atmWt[5] = 10.81;	vdwRad[5] = 192.0;	empRad[5] = 85.0;	calcRad[5] = 87.0;	//	B	boron
		atmWt[6] = 12.011;	vdwRad[6] = 170.0;	empRad[6] = 70.0;	calcRad[6] = 67.0;	//	C	carbon
		atmWt[7] = 14.007;	vdwRad[7] = 155.0;	empRad[7] = 65.0;	calcRad[7] = 56.0;	//	N	nitrogen
		atmWt[8] = 15.999;	vdwRad[8] = 152.0;	empRad[8] = 60.0;	calcRad[8] = 48.0;	//	O	oxygen
		atmWt[9] = 18.9984;	vdwRad[9] = 147.0;	empRad[9] = 50.0;	calcRad[9] = 42.0;	//	F	fluorine
		atmWt[10] = 20.1797;	vdwRad[10] = 154.0;	empRad[10] = 38.0;	calcRad[10] = 38.0;	//	Ne	neon
		atmWt[11] = 22.98976;	vdwRad[11] = 227.0;	empRad[11] = 180.0;	calcRad[11] = 190.0;	//	Na	sodium
		atmWt[12] = 24.3050;	vdwRad[12] = 173.0;	empRad[12] = 150.0;	calcRad[12] = 145.0;	//	Mg	magnesium
		atmWt[13] = 26.98153;	vdwRad[13] = 184.0;	empRad[13] = 125.0;	calcRad[13] = 118.0;	//	Al	aluminium
		atmWt[14] = 28.085;	vdwRad[14] = 210.0;	empRad[14] = 110.0;	calcRad[14] = 111.0;	//	Si	silicon
		atmWt[15] = 30.97376;	vdwRad[15] = 180.0;	empRad[15] = 100.0;	calcRad[15] = 98.0;	//	P	phosphorus
		atmWt[16] = 32.06;	vdwRad[16] = 180.0;	empRad[16] = 100.0;	calcRad[16] = 88.0;	//	S	sulfur
		atmWt[17] = 35.45;	vdwRad[17] = 175.0;	empRad[17] = 100.0;	calcRad[17] = 79.0;	//	Cl	chlorine
		atmWt[18] = 39.948;	vdwRad[18] = 188.0;	empRad[18] = 71.0;	calcRad[18] = 71.0;	//	Ar	argon
		atmWt[19] = 39.0983;	vdwRad[19] = 275.0;	empRad[19] = 220.0;	calcRad[19] = 243.0;	//	K	potassium
		atmWt[20] = 40.078;	vdwRad[20] = 231.0;	empRad[20] = 180.0;	calcRad[20] = 194.0;	//	Ca	calcium
		atmWt[21] = 44.95591;	vdwRad[21] = 211.0;	empRad[21] = 160.0;	calcRad[21] = 184.0;	//	Sc	scandium
		atmWt[22] = 47.867;	vdwRad[22] = 201.0;	empRad[22] = 140.0;	calcRad[22] = 176.0;	//	Ti	titanium
		atmWt[23] = 50.9415;	vdwRad[23] = 196.0;	empRad[23] = 135.0;	calcRad[23] = 171.0;	//	V	vanadium
		atmWt[24] = 51.9961;	vdwRad[24] = 191.0;	empRad[24] = 140.0;	calcRad[24] = 166.0;	//	Cr	chromium
		atmWt[25] = 54.93804;	vdwRad[25] = 186.0;	empRad[25] = 140.0;	calcRad[25] = 161.0;	//	Mn	manganese
		atmWt[26] = 55.845;	vdwRad[26] = 181.0;	empRad[26] = 140.0;	calcRad[26] = 156.0;	//	Fe	iron
		atmWt[27] = 58.93319;	vdwRad[27] = 177.0;	empRad[27] = 135.0;	calcRad[27] = 152.0;	//	Co	cobalt
		atmWt[28] = 58.6934;	vdwRad[28] = 163.0;	empRad[28] = 135.0;	calcRad[28] = 149.0;	//	Ni	nickel
		atmWt[29] = 63.546;	vdwRad[29] = 140.0;	empRad[29] = 135.0;	calcRad[29] = 145.0;	//	Cu	copper
		atmWt[30] = 65.38;	vdwRad[30] = 139.0;	empRad[30] = 135.0;	calcRad[30] = 142.0;	//	Zn	zinc
		atmWt[31] = 69.723;	vdwRad[31] = 187.0;	empRad[31] = 130.0;	calcRad[31] = 136.0;	//	Ga	gallium
		atmWt[32] = 72.63;	vdwRad[32] = 211.0;	empRad[32] = 125.0;	calcRad[32] = 125.0;	//	Ge	germanium
		atmWt[33] = 74.92160;	vdwRad[33] = 185.0;	empRad[33] = 115.0;	calcRad[33] = 114.0;	//	As	arsenic
		atmWt[34] = 78.96;	vdwRad[34] = 190.0;	empRad[34] = 115.0;	calcRad[34] = 103.0;	//	Se	selenium
		atmWt[35] = 79.904;	vdwRad[35] = 185.0;	empRad[35] = 115.0;	calcRad[35] = 94.0;	//	Br	bromine
		atmWt[36] = 83.798;	vdwRad[36] = 202.0;	empRad[36] = 88.0;	calcRad[36] = 88.0;	//	Kr	krypton
		atmWt[37] = 85.4678;	vdwRad[37] = 303.0;	empRad[37] = 235.0;	calcRad[37] = 265.0;	//	Rb	rubidium
		atmWt[38] = 87.62;	vdwRad[38] = 249.0;	empRad[38] = 200.0;	calcRad[38] = 219.0;	//	Sr	strontium
		atmWt[39] = 88.90585;	vdwRad[39] = 237.0;	empRad[39] = 180.0;	calcRad[39] = 212.0;	//	Y	yttrium
		atmWt[40] = 91.224;	vdwRad[40] = 231.0;	empRad[40] = 155.0;	calcRad[40] = 206.0;	//	Zr	zirconium
		atmWt[41] = 92.90638;	vdwRad[41] = 223.0;	empRad[41] = 145.0;	calcRad[41] = 198.0;	//	Nb	niobium
		atmWt[42] = 95.96;	vdwRad[42] = 215.0;	empRad[42] = 145.0;	calcRad[42] = 190.0;	//	Mo	molybdenum
		atmWt[43] = 98.0;		vdwRad[43] = 208.0;	empRad[43] = 135.0;	calcRad[43] = 183.0;	//	Tc	technetium
		atmWt[44] = 101.07;	vdwRad[44] = 203.0;	empRad[44] = 130.0;	calcRad[44] = 178.0;	//	Ru	ruthenium
		atmWt[45] = 102.9055;	vdwRad[45] = 198.0;	empRad[45] = 135.0;	calcRad[45] = 173.0;	//	Rh	rhodium
		atmWt[46] = 106.42;	vdwRad[46] = 163.0;	empRad[46] = 140.0;	calcRad[46] = 169.0;	//	Pd	palladium
		atmWt[47] = 107.8682;	vdwRad[47] = 172.0;	empRad[47] = 160.0;	calcRad[47] = 165.0;	//	Ag	silver
		atmWt[48] = 112.411;	vdwRad[48] = 158.0;	empRad[48] = 155.0;	calcRad[48] = 161.0;	//	Cd	cadmium
		atmWt[49] = 114.818;	vdwRad[49] = 193.0;	empRad[49] = 155.0;	calcRad[49] = 156.0;	//	In	indium
		atmWt[50] = 118.710;	vdwRad[50] = 217.0;	empRad[50] = 145.0;	calcRad[50] = 145.0;	//	Sn	tin
		atmWt[51] = 121.760;	vdwRad[51] = 206.0;	empRad[51] = 145.0;	calcRad[51] = 133.0;	//	Sb	antimony
		atmWt[52] = 127.60;	vdwRad[52] = 206.0;	empRad[52] = 140.0;	calcRad[52] = 123.0;	//	Te	tellurium
		atmWt[53] = 126.9044;	vdwRad[53] = 198.0;	empRad[53] = 140.0;	calcRad[53] = 115.0;	//	I	iodine
		atmWt[54] = 131.293;	vdwRad[54] = 216.0;	empRad[54] = 108.0;	calcRad[54] = 108.0;	//	Xe	xenon
		atmWt[55] = 132.9054;	vdwRad[55] = 343.0;	empRad[55] = 260.0;	calcRad[55] = 298.0;	//	Cs	caesium
		atmWt[56] = 137.327;	vdwRad[56] = 268.0;	empRad[56] = 215.0;	calcRad[56] = 253.0;	//	Ba	barium
		atmWt[57] = 138.9054;	vdwRad[57] = 250.0;	empRad[57] = 195.0;	calcRad[57] = 195.0;	//	La	lanthanum
		atmWt[58] = 140.116;	vdwRad[58] = 250.0;	empRad[58] = 185.0;	calcRad[58] = 185.0;	//	Ce	cerium
		atmWt[59] = 140.9076;	vdwRad[59] = 250.0;	empRad[59] = 185.0;	calcRad[59] = 247.0;	//	Pr	praseodymium
		atmWt[60] = 144.242;	vdwRad[60] = 250.0;	empRad[60] = 185.0;	calcRad[60] = 206.0;	//	Nd	neodymium
		atmWt[61] = 145.0;	vdwRad[61] = 250.0;	empRad[61] = 185.0;	calcRad[61] = 205.0;	//	Pm	promethium
		atmWt[62] = 150.36;	vdwRad[62] = 250.0;	empRad[62] = 185.0;	calcRad[62] = 238.0;	//	Sm	samarium
		atmWt[63] = 151.964;	vdwRad[63] = 250.0;	empRad[63] = 185.0;	calcRad[63] = 231.0;	//	Eu	europium
		atmWt[64] = 157.25;	vdwRad[64] = 250.0;	empRad[64] = 180.0;	calcRad[64] = 233.0;	//	Gd	gadolinium
		atmWt[65] = 158.9253;	vdwRad[65] = 250.0;	empRad[65] = 175.0;	calcRad[65] = 225.0;	//	Tb	terbium
		atmWt[66] = 162.500;	vdwRad[66] = 250.0;	empRad[66] = 175.0;	calcRad[66] = 228.0;	//	Dy	dysprosium
		atmWt[67] = 164.9303;	vdwRad[67] = 250.0;	empRad[67] = 175.0;	calcRad[67] = 175.0;	//	Ho	holmium
		atmWt[68] = 167.259;	vdwRad[68] = 250.0;	empRad[68] = 175.0;	calcRad[68] = 226.0;	//	Er	erbium
		atmWt[69] = 168.9342;	vdwRad[69] = 250.0;	empRad[69] = 175.0;	calcRad[69] = 222.0;	//	Tm	thulium
		atmWt[70] = 173.054;	vdwRad[70] = 250.0;	empRad[70] = 175.0;	calcRad[70] = 222.0;	//	Yb	ytterbium
		atmWt[71] = 174.9668;	vdwRad[71] = 250.0;	empRad[71] = 175.0;	calcRad[71] = 217.0;	//	Lu	lutetium
		atmWt[72] = 178.49;	vdwRad[72] = 250.0;	empRad[72] = 155.0;	calcRad[72] = 208.0;	//	Hf	hafnium
		atmWt[73] = 180.9478;	vdwRad[73] = 250.0;	empRad[73] = 145.0;	calcRad[73] = 200.0;	//	Ta	tantalum
		atmWt[74] = 183.84;	vdwRad[74] = 250.0;	empRad[74] = 135.0;	calcRad[74] = 193.0;	//	W	tungsten
		atmWt[75] = 186.207;	vdwRad[75] = 250.0;	empRad[75] = 135.0;	calcRad[75] = 188.0;	//	Re	rhenium
		atmWt[76] = 190.23;	vdwRad[76] = 250.0;	empRad[76] = 130.0;	calcRad[76] = 185.0;	//	Os	osmium
		atmWt[77] = 192.217;	vdwRad[77] = 250.0;	empRad[77] = 135.0;	calcRad[77] = 180.0;	//	Ir	iridium
		atmWt[78] = 195.084;	vdwRad[78] = 175.0;	empRad[78] = 135.0;	calcRad[78] = 177.0;	//	Pt	platinum
		atmWt[79] = 196.9665;	vdwRad[79] = 166.0;	empRad[79] = 135.0;	calcRad[79] = 174.0;	//	Au	gold
		atmWt[80] = 200.59;	vdwRad[80] = 155.0;	empRad[80] = 150.0;	calcRad[80] = 171.0;	//	Hg	mercury
		atmWt[81] = 204.38;	vdwRad[81] = 196.0;	empRad[81] = 190.0;	calcRad[81] = 156.0;	//	Tl	thallium
		atmWt[82] = 207.2;	vdwRad[82] = 202.0;	empRad[82] = 180.0;	calcRad[82] = 154.0;	//	Pb	lead
		atmWt[83] = 208.9804;	vdwRad[83] = 207.0;	empRad[83] = 160.0;	calcRad[83] = 143.0;	//	Bi	bismuth
		atmWt[84] = 209.0;	vdwRad[84] = 197.0;	empRad[84] = 190.0;	calcRad[84] = 135.0;	//	Po	polonium
		atmWt[85] = 210.0;	vdwRad[85] = 202.0;	empRad[85] = 250.0;	calcRad[85] = 250.0;	//	At	astatine
		atmWt[86] = 222.0;	vdwRad[86] = 220.0;	empRad[86] = 120.0;	calcRad[86] = 120.0;	//	Rn	radon
		atmWt[87] = 223.0;	vdwRad[87] = 348.0;	empRad[87] = 250.0;	calcRad[87] = 250.0;	//	Fr	francium
		atmWt[88] = 226.0;	vdwRad[88] = 283.0;	empRad[88] = 215.0;	calcRad[88] = 215.0;	//	Ra	radium
		atmWt[89] = 227.0;	vdwRad[89] = 250.0;	empRad[89] = 195.0;	calcRad[89] = 195.0;	//	Ac	actinium
		atmWt[90] = 232.0381;	vdwRad[90] = 250.0;	empRad[90] = 180.0;	calcRad[90] = 180.0;	//	Th	thorium
		atmWt[91] = 231.0359;	vdwRad[91] = 250.0;	empRad[91] = 180.0;	calcRad[91] = 180.0;	//	Pa	protactinium
		atmWt[92] = 238.0289;	vdwRad[92] = 186.0;	empRad[92] = 175.0;	calcRad[92] = 175.0;	//	U	uranium
		atmWt[93] = 237.0;	vdwRad[93] = 225.0;	empRad[93] = 175.0;	calcRad[93] = 175.0;	//	Np	neptunium
		atmWt[94] = 244.0;	vdwRad[94] = 250.0;	empRad[94] = 175.0;	calcRad[94] = 175.0;	//	Pu	plutonium
		atmWt[95] = 243.0;	vdwRad[95] = 225.0;	empRad[95] = 175.0;	calcRad[95] = 175.0;	//	Am	americium
		atmWt[96] = 247.0;	vdwRad[96] = 250.0;												//	Cm	curium
		atmWt[97] = 247.0;	vdwRad[97] = 250.0;												//	Bk	berkelium
		atmWt[98] = 251.0;	vdwRad[98] = 250.0;												//	Cf	californium
		atmWt[99] = 252.0;	vdwRad[99] = 250.0;												//	Es	einsteinium
		atmWt[100] = 257.0;																	//Fm Fermium		
		atmWt[101] = 258.0;																	//Md Mendelevium		
		atmWt[102] = 259.0;																	//No Nobelium		
		atmWt[103] = 262.0;																	//Lr Lawrencium		
		atmWt[104] = 265.0;																	//Rf Rutherfordium		
		atmWt[105] = 268.0;																	//Db Dubnium		
		atmWt[106] = 271.0;																	//Sg Seaborgium		
		atmWt[107] = 270.0;																	//Bh Bohrium		
		atmWt[108] = 277.0;																	//Hs Hassium		
		atmWt[109] = 276.0;																	//Mt Meitnerium		
		atmWt[110] = 281.0;																	//Ds Darmstadtium		
		atmWt[111] = 280.0;																	//Rg	Roentgenium	
		atmWt[112] = 285.0;																	//Cn Copernicium		
		atmWt[113] = 284.0;																	//Uut	Ununtrium	
		atmWt[114] = 289.0;																	//Uuq Ununquadium		
		atmWt[115] = 288.0;																	//Uup Ununpentium		
		atmWt[116] = 293.0;																	//Uuh Ununhexium		
		atmWt[117] = 294.0;																	//Uus Ununseptium		
		atmWt[118] = 294.0;																	//Uuo Ununoctium		
#pragma endregion

		for (unsigned int i = 0; i < vdwRad.size(); i++) {
			vdwRad[i] = vdwRad[i] / 1000.0;	// pm --> nm
			empRad[i] = empRad[i] / 1000.0;	// pm --> nm
			calcRad[i] = calcRad[i] / 1000.0;	// pm --> nm
		}

		svgRad = empRad;
		svgRad[1] = 0.107;
		svgRad[6] = 0.1577;
		svgRad[7] = 0.08414;
		svgRad[8] = 0.130;
		svgRad[16] = 0.168;
		svgRad[12] = 0.160;
		svgRad[15] = 0.111;
		svgRad[20] = 0.197;
		svgRad[25] = 0.130;
		svgRad[26] = 0.124;
		svgRad[29] = 0.128;
		svgRad[30] = 0.133;

		//////////////////////////////////////////////////////////////////////////
		// Atomic groups (CH, NH3, etc.)

		// CH CH2 CH3
		atmWt[119] = 1 * atmWt[0] + atmWt[6];
		atmWt[120] = 2 * atmWt[0] + atmWt[6];
		atmWt[121] = 3 * atmWt[0] + atmWt[6];
		vdwRad[119] = cbrt(vdwRad[6] * vdwRad[6] * vdwRad[6] + vdwRad[0] * vdwRad[0] * vdwRad[0]);
		vdwRad[120] = cbrt(vdwRad[6] * vdwRad[6] * vdwRad[6] + 2. * vdwRad[0] * vdwRad[0] * vdwRad[0]);
		vdwRad[121] = cbrt(vdwRad[6] * vdwRad[6] * vdwRad[6] + 3. * vdwRad[0] * vdwRad[0] * vdwRad[0]);
		empRad[119] = cbrt(empRad[6] * empRad[6] * empRad[6] + empRad[0] * empRad[0] * empRad[0]);
		empRad[120] = cbrt(empRad[6] * empRad[6] * empRad[6] + 2. * empRad[0] * empRad[0] * empRad[0]);
		empRad[121] = cbrt(empRad[6] * empRad[6] * empRad[6] + 3. * empRad[0] * empRad[0] * empRad[0]);
		calcRad[119] = cbrt(calcRad[6] * calcRad[6] * calcRad[6] + calcRad[0] * calcRad[0] * calcRad[0]);
		calcRad[120] = cbrt(calcRad[6] * calcRad[6] * calcRad[6] + 2. * calcRad[0] * calcRad[0] * calcRad[0]);
		calcRad[121] = cbrt(calcRad[6] * calcRad[6] * calcRad[6] + 3. * calcRad[0] * calcRad[0] * calcRad[0]);
		svgRad[119] = 0.173;
		svgRad[120] = 0.185;
		svgRad[121] = 0.197;

		// NH NH2 NH3
		atmWt[122] = 1 * atmWt[0] + atmWt[7];
		atmWt[123] = 2 * atmWt[0] + atmWt[7];
		atmWt[124] = 3 * atmWt[0] + atmWt[7];
		vdwRad[122] = cbrt(vdwRad[7] * vdwRad[7] * vdwRad[7] + vdwRad[0] * vdwRad[0] * vdwRad[0]);
		vdwRad[123] = cbrt(vdwRad[7] * vdwRad[7] * vdwRad[7] + 2. * vdwRad[0] * vdwRad[0] * vdwRad[0]);
		vdwRad[124] = cbrt(vdwRad[7] * vdwRad[7] * vdwRad[7] + 3. * vdwRad[0] * vdwRad[0] * vdwRad[0]);
		empRad[122] = cbrt(empRad[7] * empRad[7] * empRad[7] + empRad[0] * empRad[0] * empRad[0]);
		empRad[123] = cbrt(empRad[7] * empRad[7] * empRad[7] + 2. * empRad[0] * empRad[0] * empRad[0]);
		empRad[124] = cbrt(empRad[7] * empRad[7] * empRad[7] + 3. * empRad[0] * empRad[0] * empRad[0]);
		calcRad[122] = cbrt(calcRad[7] * calcRad[7] * calcRad[7] + calcRad[0] * calcRad[0] * calcRad[0]);
		calcRad[123] = cbrt(calcRad[7] * calcRad[7] * calcRad[7] + 2. * calcRad[0] * calcRad[0] * calcRad[0]);
		calcRad[124] = cbrt(calcRad[7] * calcRad[7] * calcRad[7] + 3. * calcRad[0] * calcRad[0] * calcRad[0]);
		svgRad[122] = 0.122;
		svgRad[123] = 0.145;
		svgRad[124] = 0.162;

		// OH
		atmWt[125] = 1 * atmWt[0] + atmWt[8];
		vdwRad[125] = cbrt(vdwRad[8] * vdwRad[8] * vdwRad[8] + vdwRad[0] * vdwRad[0] * vdwRad[0]);
		empRad[125] = cbrt(empRad[8] * empRad[8] * empRad[8] + 2. * empRad[0] * empRad[0] * empRad[0]);
		calcRad[125] = cbrt(calcRad[8] * calcRad[8] * calcRad[8] + 3. * calcRad[0] * calcRad[0] * calcRad[0]);
		svgRad[125] = 0.150;

		// SH
		atmWt[126] = 1 * atmWt[0] + atmWt[16];
		vdwRad[126] = cbrt(vdwRad[16] * vdwRad[16] * vdwRad[16] + vdwRad[0] * vdwRad[0] * vdwRad[0]);
		empRad[126] = cbrt(empRad[16] * empRad[16] * empRad[16] + 2. * empRad[0] * empRad[0] * empRad[0]);
		calcRad[126] = cbrt(calcRad[16] * calcRad[16] * calcRad[16] + 3. * calcRad[0] * calcRad[0] * calcRad[0]);
		svgRad[126] = 0.181;



		// Load atomic form factor coefficients (KEEP IN ONE PLACE! here is better)
		// Atomic values of Table 4.3.2.2 (International Tables of X-ray Crystallography Vol IV)
		// Ionic values come from Peng 1998
		// They are good up to s < 2.0A
		atmFFcoefs.resize(NUMBER_OF_ATOMIC_FORM_FACTORS * 10);
		Eigen::Map<Eigen::Array<FLOAT_TYPE, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> atmFFcoefsMap(atmFFcoefs.data(), NUMBER_OF_ATOMIC_FORM_FACTORS, 10);

#pragma region Atomic form factor coefficients
		atmFFcoefsMap << 0.0349, 0.5347, 0.1201, 3.5867, 0.1970, 12.3471, 0.0573, 18.9525, 0.1195, 38.6269,	// H
			0.0317, 0.2507, 0.0838, 1.4751, 0.1526, 4.4938, 0.1334, 12.6646, 0.0164, 31.1653,	// He
			0.0750, 0.3864, 0.2249, 2.9383, 0.5548, 15.3829, 1.4954, 53.5545, 0.9354, 138.7337,	// Li
			0.00460, 0.0358, 0.0165, 0.239, 0.0435, 0.879, 0.0649, 2.64, 0.0270, 7.09, 			//Li+1
			0.0780, 0.3131, 0.2210, 2.2381, 0.6740, 10.1517, 1.3867, 30.9061, 0.6925, 78.3273,	// Be
			0.00340, 0.0267, 0.0103, 0.162, 0.0233, 0.531, 0.0325, 1.48, 0.0120, 3.88, 			//Be+2
			0.0909, 0.2995, 0.2551, 2.1155, 0.7738, 8.3816, 1.2136, 24.1292, 0.4606, 63.1314,	// B
			0.0893, 0.2465, 0.2563, 1.7100, 0.7570, 6.4094, 1.0487, 18.6113, 0.3575, 50.2523,	// C
			0.1022, 0.2451, 0.3219, 1.7481, 0.7982, 6.1925, 0.8197, 17.3894, 0.1715, 48.1431,	// N
			0.0974, 0.2067, 0.2921, 1.3815, 0.6910, 4.6943, 0.6990, 12.7105, 0.2039, 32.4726,	// O
			0.205, 0.397, 0.628, 0.264, 1.17, 8.80, 1.03, 27.1, 0.290, 91.8, 					//O-1
			0.1083, 0.2057, 0.3175, 1.3439, 0.6487, 4.2788, 0.5846, 11.3932, 0.1421, 28.7881,	// F
			0.134, 0.228, 0.391, 1.47, 0.814, 4.68, 0.928, 13.2, 0.347, 36.0, 					//F-1
			0.1269, 0.2200, 0.3535, 1.3779, 0.5582, 4.0203, 0.4674, 9.4934, 0.1460, 23.1278,	// Ne
			0.2142, 0.334, 0.6853, 2.3446, 0.7692, 10.0830, 1.6589, 48.3037, 1.4482, 137.2700,	// Na
			0.0256, 0.0397, 0.0919, 0.287, 0.297, 1.18, 0.514, 3.75, 0.199, 10.8,				// Na+1
			0.2314, 0.3278, 0.6866, 2.2720, 0.9677, 10.9241, 2.1182, 32.2898, 1.1339, 101.9748,	// Mg
			0.0210, 0.0331, 0.0672, 0.222, 0.198, 0.838, 0.368, 2.48, 0.174, 6.75,				// Mg+2
			0.2390, 0.3138, 0.6573, 2.1063, 1.2011, 10.4163, 2.5586, 34.4552, 1.2312, 98.5344,	// Al
			0.0192, 0.0306, 0.0579, 0.198, 0.163, 0.713, 0.284, 2.04, 0.114, 5.25,				// Al+3
			0.2519, 0.3075, 0.6372, 2.0174, 1.3795, 9.6746, 2.5082, 29.3744, 1.0500, 80.4732,	// Si_v
			0.192, 0.359, 0.289, 1.96, 0.100, 9.34, -0.0728, 11.1, 0.00120, 13.4,				// Si+4
			0.2548, 0.2908, 0.6106, 1.8740, 1.4541, 8.5176, 2.3204, 24.3434, 0.8477, 63.2996,	// P
			0.2497, 0.2681, 0.5628, 1.6711, 1.3899, 7.0267, 2.1865, 19.5377, 0.7715, 50.3888,	// S
			0.2443, 0.2468, 0.5397, 1.5242, 1.3919, 6.1537, 2.0197, 16.6687, 0.6621, 42.3086,	// Cl
			0.265, 0.252, 0.596, 1.56, 1.60, 6.21, 2.69, 17.8, 1.23, 47.8,						// Cl-1
			0.2385, 0.2289, 0.5017, 1.3694, 1.3128, 5.2561, 1.8899, 14.0928, 0.6079, 35.5361,	//Ar
			0.4115, 0.3703, 1.4031, 3.3874, 2.2784, 13.1029, 2.6742, 68.9592, 2.2162, 194.4329,	//K
			0.199, 0.192, 0.396, 1.10, 0.928, 3.91, 1.45, 9.75, 0.450, 23.4,					//K+1
			0.4054, 0.3499, 1.880, 3.0991, 2.1602, 11.9608, 3.7532, 53.9353, 2.2063, 142.3892,	//Ca
			0.164, 0.157, 0.327, 0.894, 0.743, 3.15, 1.16, 7.67, 0.307, 17.7,					//Ca+2
			0.3787, 0.3133, 1.2181, 2.5856, 2.0594, 9.5813, 3.2618, 41.7688, 2.3870, 116.7282,	//Sc
			0.163, 0.157, 0.307, 0.899, 0.716, 3.06, 0.880, 7.05, 0.139, 16.1,					//Sc+3
			0.3825, 0.3040, 1.2598, 2.4863, 2.0008, 9.2783, 3.0617, 39.0751, 2.0694, 109.4583,	//Ti
			0.399, 0.376, 1.04, 2.74, 1.21, 8.10, -0.0797, 14.2, 0.352, 23.2,					//Ti+2
			0.364, 0.364, 0.919, 2.67, 1.35, 8.18, -0.933, 11.8, 0.589, 14.9,					//Ti+3
			0.116, 0.108, 0.256, 0.655, 0.565, 2.38, 0.772, 5.51, 0.32, 12.3,					//Ti+4
			0.3876, 0.2967, 1.2750, 2.3780, 1.9109, 8.7981, 2.8314, 35.9528, 1.8979, 101.7201,	//V
			0.317, 0.269, 0.939, 2.09, 1.49, 7.22, -1.31, 15.2, 1.47, 17.6,						//V+2
			0.341, 0.321, 0.805, 2.23, 0.942, 5.99, 0.0783, 13.4, 0.156, 16.9,					//V+3
			0.0367, 0.0330, 0.124, 0.222, 0.244, 0.824, 0.723, 2.8, 0.435, 6.70,				//V+5
			0.4046, 0.2986, 1.3696, 2.3958, 1.8941, 9.1406, 2.0800, 37.4701, 1.2196, 113.7121,	//Cr
			0.237, 0.177, 0.634, 1.35, 1.23, 4.30, 0.713, 12.2, 0.0859, 39.0,					//Cr+2
			0.393, 0.359, 1.05, 2.57, 1.62, 8.68, -1.15, 11.0, 0.407, 15.8,						//Cr+3
			0.3796, 0.2699, 1.2094, 2.0455, 1.7815, 7.4726, 2.5420, 31.0604, 1.5937, 91.5622,	//Mn
			0.0576, 0.0398, 0.210, 0.284, 0.604, 1.29, 1.32, 4.23, 0.659, 14.5,					//Mn+2
			0.116, 0.0117, 0.523, 0.876, 0.881, 3.06, 0.589, 6.44, 0.214, 14.3,					//Mn+3
			0.381, 0.354, 1.83, 2.72, -1.33, 3.47, 0.995, 5.47, 0.0618, 16.1,					//Mn+4
			0.3946, 0.2717, 1.2725, 2.0443, 1.7031, 7.6007, 2.3140, 29.9714, 1.4795, 86.2265,	//Fe
			0.307, 0.230, 0.838, 1.62, 1.11, 4.87, 0.280, 10.7, 0.277, 19.2,					//Fe+2
			0.198, 0.154, 0.384, 0.893, 0.889, 2.62, 0.709, 6.65, 0.117, 18.0,				 	//Fe+3
			0.4118, 0.2742, 1.3161, 2.0372, 1.6493, 7.7205, 2.1930, 29.9680, 1.2830, 84.9383,	//Co
			0.213, 0.148, 0.488, 0.939, 0.998, 2.78, 0.828, 7.31, 0.230, 20.7,					//Co+2
			0.331, 0.267, 0.487, 1.41, 0.729, 2.89, 0.608, 6.45, 0.131, 15.8,					//Co+3
			0.3860, 0.2478, 1.1765, 1.7660, 1.5451, 6.3107, 2.0730, 25.2204, 1.3814, 74.3146,	//Ni
			0.338, 0.237, 0.982, 1.67, 1.32, 5.73, -3.56, 11.4, 3.62, 12.1,						//Ni+2
			0.347, 0.260, 0.877, 1.71, 0.790, 4.75, 0.0538, 7.51, 0.192, 13.0,					//Ni+3
			0.4314, 0.2694, 1.3208, 1.9223, 1.5236, 7.3474, 1.4671, 28.9892, 0.8562, 90.6246,	//Cu
			0.312, 0.201, 0.812, 1.31, 1.11, 3.80, 0.794, 10.5, 0.257, 28.2,					//Cu+1
			0.224, 0.145, 0.544, 0.933, 0.970, 2.69, 0.727, 7.11, 0.1882, 19.4,					//Cu+2
			0.4288, 0.2593, 1.2646, 1.7998, 1.4472, 6.7500, 1.8294, 25.5860, 1.0934, 73.5284,	//Zn
			0.252, 0.161, 0.600, 1.01, 0.917, 2.76, 0.663, 7.08, 0.161, 19.0,					//Zn+2
			0.4818, 0.2825, 1.4032, 1.9785, 1.6561, 8.7546, 2.4605, 32.5238, 1.1054, 98.5523,	//Ga
			0.391, 0.264, 0.947, 1.65, 0.690, 4.82, 0.0709, 10.7, 0.0653, 15.2,					//Ga+3
			0.4655, 0.2647, 1.3014, 1.7926, 1.6088, 7.6071, 2.6998, 26.5541, 1.3003, 77.5238,	//Ge
			0.346, 0.232, 0.830, 1.45, 0.599, 4.08, 0.0949, 13.2, -0.0217, 29.5,				//Ge+4
			0.4517, 0.2493, 1.2229, 1.6436, 1.5852, 6.8154, 2.7958, 22.3681, 1.2638, 62.0390,	//As
			0.4477, 0.2405, 1.1678, 1.5442, 1.5843, 6.3231, 2.8087, 19.4610, 1.1956, 52.0233,	//Se
			0.4798, 0.2504, 1.1948, 1.5963, 1.8695, 6.9653, 2.6953, 19.8492, 0.8203, 50.3233,	//Br
			0.125, 0.0530, 0.563, 0.469, 1.43, 2.15, 3.25, 11.1, 3.22, 38.9,					//Br-1
			0.4546, 0.2309, 1.0993, 1.4279, 1.76966, 5.9449, 2.7068, 16.6752, 0.8672, 42.2243,	//Kr
			1.0160, 0.4853, 2.8528, 5.0925, 3.5466, 25.7851, -7.7804, 130.4515, 12.1148, 138.6775,//Rb
			0.368, 0.187, 0.884, 1.12, 1.12, 3.98, 2.26, 10.9, 0.881, 26.6,						//Rb+1
			0.6703, 0.3190, 1.4926, 2.2287, 3.3368, 10.3504, 4.4600, 52.3291, 3.1501, 151.2216,	//Sr
			0.346, 0.176, 0.804, 1.04, 0.988, 3.59, 1.89, 9.32, 0.609, 21.4,					//Sr+2
			0.6894, 0.3189, 1.5474, 2.2904, 3.2450, 10.0062, 4.2126, 44.0771, 2.9764, 125.0120,	//Y
			0.465, 0.240, 0.923, 1.43, 2.41, 6.45, -2.31, 9.97, 2.48, 12.2,						//Y+3
			0.6719, 0.3036, 1.4684, 2.1249, 3.1668, 8.9236, 3.9957, 36.8458, 2.8920, 108.2049,	//Zr
			0.34, 0.113, 0.642, 0.736, 0.747, 2.54, 1.47, 6.72, 0.377, 14.7,					//Zr+4
			0.6123, 0.2709, 1.2677, 1.7683, 3.0348, 7.2489, 3.3841, 27.9465, 2.3683, 98.5624,	//Nb
			0.377, 0.184, 0.749, 1.02, 1.29, 3.80, 1.61, 9.44, 0.481, 25.7,						//Nb+3
			0.0828, 0.0369, 0.271, 0.261, 0.654, 0.957, 1.24, 3.94, 0.829, 9.44,				//Nb+5
			0.6773, 0.2920, 1.4798, 2.0606, 3.1788, 8.1129, 3.0824, 30.5336, 1.8384, 100.0658,	//Mo
			0.401, 0.191, 0.756, 1.06, 1.38, 3.84, 1.58, 9.38, 0.497, 24.6,						//Mo+3
			0.479, 0.241, 0.846, 1.46, 15.6, 6.79, -15.2, 7.13, 1.60, 10.4,						//Mo+5
			0.203, 0.0971, 0.567, 0.647, 0.646, 2.28, 1.16, 5.61, 0.171, 12.4,					//Mo+6
			0.7082, 0.2976, 1.6392, 2.2106, 3.1993, 8.5246, 3.4327, 33.1456, 1.8711, 96.6377,	//Tc
			0.6735, 0.2773, 1.4934, 1.9716, 3.0966, 7.3249, 2.7254, 26.6891, 1.5597, 90.5581,	//Ru
			0.428, 0.191, 0.773, 1.09, 1.55, 3.82, 1.46, 9.08, 0.486, 21.7,						//Ru+3
			0.2882, 0.125, 0.653, 0.753, 1.14, 2.85, 1.53, 7.01, 0.418, 17.5,					//Ru+4
			0.6413, 0.2580, 1.3690, 1.7721, 2.9854, 6.3854, 2.6952, 23.2549, 1.5433, 58.1517,	//Rh
			0.352, 0.151, 0.723, 0.878, 1.50, 3.28, 1.63, 8.16, 0.499, 20.7,					//Rh+3
			0.397, 0.177, 0.725, 1.01, 1.51, 3.62, 1.19, 8.56, 0.251, 18.9,						//Rh+4
			0.5904, 0.2324, 1.1775, 1.5019, 2.6519, 5.1591, 2.2875, 15.5428, 0.8689, 46.8213,	//Pd
			0.935, 0.393, 3.11, 4.06, 24.6, 43.1, -43.6, 54.0, 21.2, 69.8,						//Pd+2
			0.348, 0.151, 0.640, 0.832, 1.22, 2.85, 1.45, 6.59, 0.427, 15.6,					//Pd+4
			0.6377, 0.2466, 1.3790, 1.6974, 2.8294, 5.7656, 2.3631, 20.0943, 1.4553, 76.7372,	//Ag
			0.503, 0.199, 0.940, 1.19, 2.17, 4.05, 1.99, 11.3, 0.726, 32.4,						//Ag+1
			0.431, 0.175, 0.756, 0.979, 1.72, 3.30, 1.78, 8.24, 0.526, 21.4,					//Ag+2
			0.6364, 0.2407, 1.4247, 1.6823, 2.7802, 5.6588, 2.5973, 20.7219, 1.7886, 69.1109,	//Cd
			0.425, 0.168, 0.745, 0.944, 1.73, 3.14, 1.74, 7.84, 0.487, 20.4,					//Cd+2
			0.6768, 0.2522, 1.6589, 1.8545, 2.7740, 6.2936, 3.1835, 25.1457, 2.1326, 84.5448,	//In
			0.417, 0.164, 0.755, 0.960, 1.59, 3.08, 1.36, 7.03, 0.451, 16.1,					//In+3
			0.7224, 0.2651, 1.9610, 2.0604, 2.7161, 7.3011, 3.5603, 27.5493, 1.8972, 81.3349,	//Sn
			0.797, 0.317, 2.13, 2.51, 2.15, 9.04, -1.64, 24.2, 2.72, 26.4,						//Sn+2
			0.261, 0.0957, 0.642, 0.625, 1.53, 2.51, 1.36, 6.31, 0.177, 15.9,					//Sn+4
			0.7106, 0.2562, 1.9247, 1.9646, 2.6149, 6.8852, 3.8322, 24.7648, 1.8899, 68.9168,	//Sb
			0.552, 0.212, 1.14, 1.42, 1.87, 4.21, 1.36, 12.5, 0.414, 29.0,						//Sb+3
			0.377, 0.151, 0.588, 0.812, 1.22, 2.40, 1.18, 5.27, 0.244, 11.9,					//Sb+5
			0.6947, 0.2459, 1.8690, 1.8542, 2.5356, 6.4411, 4.0013, 22.1730, 1.8955, 59.2206,	//Te
			0.7047, 0.2455, 1.9484, 1.8638, 2.5940, 6.7639, 4.1526, 21.8007, 1.5057, 56.4395,	//I
			0.901, 0.312, 2.80, 2.59, 5.61, 4.1, -8.69, 34.4, 12.6, 39.5,						//I-1
			0.6737, 0.2305, 1.7908, 1.6890, 2.4129, 5.8218, 4.5100, 18.3928, 1.7058, 47.2496,	//Xe
			1.2704, 0.4356, 3.8018, 4.2058, 5.6618, 23.4342, 0.9205, 136.7783, 4.8105, 171.7561,//Cs
			0.587, 0.200, 1.40, 1.38, 1.87, 4.12, 3.48, 13.0, 1.67, 31.8,						//Cs+1
			0.9049, 0.3066, 2.6076, 2.4363, 4.8498, 12.1821, 5.1603, 54.6135, 4.7388, 161.9978,	//Ba
			0.733, 0.258, 2.05, 1.96, 23.0, 11.8, -152, 14.4, 134, 14.9,						//Ba+2
			0.8405, 0.2791, 2.3863, 2.1410, 4.6139, 10.3400, 4.1514, 41.9148, 4.7949, 132.0204,	//La
			0.493, 0.167, 1.10, 1.11, 1.50, 3.11, 2.70, 9.61, 1.08, 21.2,						//La+3
			0.8551, 0.2805, 2.3915, 2.1200, 4.5772, 10.1808, 5.0278, 42.0633, 4.5118, 130.9893,	//Ce
			0.560, 0.190, 1.35, 1.30, 1.59, 3.93, 2.63, 10.7, 0.706, 23.8,						//Ce+3
			0.483, 0.165, 1.09, 1.10, 1.34, 3.02, 2.45, 8.85, 0.797, 18.8,						//Ce+4
			0.9096, 0.2939, 2.5313, 2.2471, 4.5266, 10.8266, 4.6376, 48.8842, 4.3690, 147.6020,	//Pr
			0.663, 0.226, 1.73, 1.61, 2.35, 6.33, 0.351, 11.0, 1.59, 16.9,						//Pr+3
			0.512, 0.177, 1.19, 1.17, 1.33, 3.28, 2.36, 8.94, 0.690, 19.3,						//Pr+4
			0.8807, 0.2802, 2.4183, 2.0836, 4.4448, 10.0357, 4.6858, 47.4506, 4.1725, 146.9976,	//Nd
			0.501, 0.162, 1.18, 1.08, 1.45, 3.06, 2.53, 8.80, 0.920, 19.6,						//Nd+3
			0.9471, 0.2977, 2.5463, 2.2276, 4.3523, 10.5762, 4.4789, 49.3619, 3.9080, 145.3580,	//Pm
			0.496, 0.156, 1.20, 1.05, 1.47, 3.07, 2.43, 8.56, 0.943, 19.2,						//Pm+3
			0.9699, 0.3003, 2.5837, 2.2447, 4.2778, 10.6487, 4.4575, 50.7994, 3.5985, 146.4176,	//Sm
			0.518, 0.163, 1.24, 1.08, 1.43, 3.11, 2.40, 8.52, 0.781, 19.1,						//Sm+3
			0.8694, 0.2653, 2.2413, 1.8590, 3.9196, 8.3998, 3.9694, 36.7397, 4.5498, 125.7089,	//Eu
			0.613, 0.190, 1.53, 1.27, 1.84, 4.18, 2.46, 10.7, 0.714, 26.2,						//Eu+2
			0.496, 0.152, 1.21, 1.01, 1.45, 2.95, 2.36, 8.18, 0.774, 18.5,						//Eu+3
			0.9673, 0.2909, 2.4702, 2.1014, 4.1148, 9.7067, 4.4972, 43.4270, 3.2099, 125.9474,	//Gd
			0.490, 0.148, 1.19, 0.974, 1.42, 2.81, 2.30, 7.78, 0.795, 17.7,						//Gd+3
			0.9325, 0.2761, 2.3673, 1.9511, 3.8791, 8.9296, 3.9674, 41.5937, 3.7996, 131.0122,	//Tb
			0.503, 0.150, 1.22, 0.982, 1.42, 2.86, 2.24, 7.77, 0.710, 17.7,						//Tb+3
			0.9505, 0.2773, 2.3705, 1.9469, 3.8218, 8.8862, 4.0471, 43.0938, 3.4451, 133.1396,	//Dy
			0.503, 0.148, 1.24, 0.970, 1.44, 2.88, 2.17, 7.73, 0.643, 17.6,						//Dy+3
			0.9248, 0.2660, 2.2428, 1.8183, 3.6182, 7.9655, 3.7910, 33.1129, 3.7912, 101.8139,	//Ho
			0.456, 0.129, 1.17, 0.869, 1.43, 2.61, 2.15, 7.24, 0.692, 16.7,						//Ho+3
			1.0373, 0.2944, 2.4824, 2.0797, 3.6558, 9.4156, 3.8925, 45.8056, 3.0056, 132.7720,	//Er
			0.522, 0.150, 1.28, 0.964, 1.46, 2.93, 2.05, 7.72, 0.508, 17.8,						//Er+3
			1.0075, 0.2816, 2.3787, 1.9486, 3.5440, 8.7162, 3.6932, 41.8420, 3.1759, 125.0320,	//Tm
			0.475, 0.132, 1.20, 0.864, 1.42, 2.60, 2.05, 7.09, 0.584, 16.6,						//Tm+3
			1.0347, 0.2855, 2.3911, 1.9679, 3.4619, 8.7619, 3.6556, 42.3304, 3.0052, 125.6499,	//Yb
			0.508, 0.136, 1.37, 0.922, 1.76, 3.12, 2.23, 8.722, 0.584, 23.7,					//Yb+2
			0.498, 0.138, 1.22, 0.881, 1.39, 2.63, 1.97, 6.99, 0.559, 16.3,						//Yb+3
			0.9927, 0.2701, 2.2436, 1.8073, 3.3554, 7.8112, 3.7813, 34.4849, 3.0994, 103.3526,	//Lu
			0.483, 0.131, 1.21, 0.845, 1.41, 2.57, 1.94, 6.88, 0.522, 16.2,						//Lu+3
			1.0295, 0.2761, 2.2911, 1.8625, 3.4110, 8.0961, 3.9497, 34.2712, 2.4925, 98.5295,	//Hf
			0.522, 0.145, 1.22, 0.896, 1.37, 2.74, 1.68, 6.91, 0.312, 16.1,						//Hf+4
			1.0190, 0.2694, 2.2291, 1.7962, 3.4097, 7.6944, 3.9252, 31.0942, 2.2679, 91.1089,	//Ta
			0.569, 0.161, 1.26, 0.972, 0.979, 2.76, 1.29, 5.40, 0.551, 10.9,					//Ta+5
			0.9853, 0.2569, 2.1167, 1.6745, 3.3570, 7.0098, 3.7981, 26.9234, 2.2798, 81.3910,	//W
			0.181, 0.0118, 0.873, 0.442, 1.18, 1.52, 1.48, 4.35, 0.56, 9.42,					//W+6
			0.9914, 0.2548, 2.0858, 1.6518, 3.4531, 6.8845, 3.8812, 26.7234, 1.8526, 81.7215,	//Re
			0.9813, 0.2487, 2.0322, 1.5973, 3.3665, 6.4737, 3.6235, 23.2817, 1.9741, 70.9254,	//Os
			0.586, 0.155, 1.31, 0.938, 1.63, 3.19, 1.71, 7.84, 0.540, 19.3,						//Os+4
			1.0194, 0.2554, 2.0645, 1.6475, 3.4425, 6.5966, 3.4914, 23.2269, 1.6976, 70.0272,	//Ir
			0.692, 0.182, 1.37, 1.04, 1.80, 3.47, 1.97, 8.51, 0.804, 21.2,						//Ir+3
			0.653, 0.174, 1.29, 0.992, 1.50, 3.14, 1.74, 7.22, 0.683, 17.2,						//Ir+4
			0.9148, 0.2263, 1.8096, 1.3813, 3.2134, 5.3243, 3.2953, 17.5987, 1.5754, 60.0171,	//Pt
			0.872, 0.223, 1.68, 1.35, 2.63, 4.99, 1.93, 13.6, 0.475, 33.0,						//Pt+2
			0.550, 0.142, 1.21, 0.833, 1.62, 2.81, 1.95, 7.21, 0.610, 17.7,						//Pt+4
			0.9674, 0.2358, 1.8916, 1.4712, 3.3993, 5.6758, 3.0524, 18.7119, 1.2607, 61.5286,	//Au
			0.811, 0.201, 1.57, 1.18, 2.63, 4.25, 2.68, 12.1, 0.998, 34.4,						//Au+1
			0.722, 0.184, 1.39, 1.06, 1.94, 3.58, 1.94, 8.56, 0.699, 20.4,						//Au+3
			1.0033, 0.2413, 1.9469, 1.5298, 3.4396, 5.8009, 3.1548, 19.4520, 1.4180, 60.5753,	//Hg
			0.796, 0.194, 1.56, 1.14, 2.72, 4.21, 2.76, 12.4, 1.18, 36.2,						//Hg+1
			0.773, 0.191, 1.49, 1.12, 2.45, 4.00, 2.23, 1.08, 0.570, 27.6,						//Hg+2
			1.0689, 0.2540, 2.1038, 1.6715, 3.6039, 6.3509, 3.4927, 23.1531, 1.8283, 78.7099,	//Tl
			0.820, 0.197, 1.57, 1.16, 2.78, 4.23, 2.82, 12.7, 1.31, 35.7,						//Tl+1
			0.836, 0.208, 1.43, 1.20, 0.394, 2.57, 2.51, 4.86, 1.50, 13.5,						//Tl+3
			1.0891, 0.2552, 2.1867, 1.7174, 3.6160, 6.5131, 3.8031, 23.9170, 1.8994, 74.7039,	//Pb
			0.755, 0.181, 1.44, 1.05, 2.48, 3.75, 2.45, 10.6, 1.03, 27.9,						//Pb+2
			0.583, 0.144, 1.14, 0.796, 1.60, 2.58, 2.06, 6.22, 0.662, 14.8,						//Pb+4
			1.1007, 0.2546, 2.2306, 1.7351, 3.5689, 6.4948, 4.1549, 23.6464, 2.0382, 70.3780,	//Bi
			0.708, 0.170, 1.35, 0.981, 2.28, 3.44, 2.18, 9.41, 0.797, 23.7,						//Bi+3
			0.654, 0.162, 1.18, 0.905, 1.25, 2.68, 1.66, 5.14, 0.778, 11.2,						//Bi+5
			1.1568, 0.2648, 2.4353, 1.8786, 3.6459, 7.1749, 4.4064, 25.1766, 1.7179, 69.2821,	//Po
			1.0909, 0.2466, 2.1976, 1.6707, 3.3831, 6.0197, 4.6700, 207657, 2.1277, 57.2663,	//At
			1.0756, 0.2402, 2.1630, 1.6169, 3.3178, 5.7644, 4.8852, 19.4568, 2.0489, 52.5009,	//Rn
			1.4282, 0.3183, 3.5081, 2.6889, 5.6767, 13.4816, 4.1964, 54.3866, 3.8946, 200.8321,	//Fr
			1.3127, 0.2887, 3.1243, 2.2897, 5.2988, 10.8276, 5.3891, 43.5389, 5.4133, 145.6109,	//Ra
			0.911, 0.204, 1.65, 1.26, 2.53, 1.03, 3.62, 12.6, 1.58, 30.0,						//Ra+2
			1.3128, 0.2861, 3.1021, 2.2509, 5.3385, 10.5287, 5.9611, 41.7796, 4.7562, 128.2973,	//Ac
			0.915, 0.205, 1.64, 1.28, 2.26, 3.92, 3.18, 11.3, 1.25, 25.1,						//Ac+3
			1.2553, 0.2701, 2.9178, 2.0636, 5.0862, 9.3051, 6.1206, 34.5977, 4.7122, 107.9200,	//Th
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 														//Th+4
			1.3218, 0.2827, 3.1444, 2.2250, 5.4371, 10.2454, 5.6444, 41.1162, 4.0107, 124.4449,	//Pa
			1.3382, 0.2838, 3.2043, 2.2452, 5.4558, 10.2519, 5.4839, 41.7251, 3.6342, 124.9023,	//U
			1.14, 0.250, 2.48, 1.84, 3.61, 7.39, 1.13, 18.0, 0.900, 22.7,						//U+3
			1.09, 0.243, 2.32, 1.75, 12.0, 7.79, -9.11, 8.31, 2.15, 16.5,						//U+4
			0.687, 0.154, 1.14, 0.861, 1.83, 2.58, 2.53, 7.70, 0.957, 15.9,						//U+6
			1.5193, 0.3213, 4.0053, 2.8206, 6.5327, 14.8878, -0.1402, 68.9103, 6.7489, 81.7257,	//Np
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Np+3
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Np+4
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Np+6
			1.3517, 0.2813, 3.2937, 2.2418, 5.3212, 9.9952, 4.6466, 42.7939, 3.5714, 132.1739,	//Pu
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Pu+3
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Pu+4
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0,														//Pu+6
			1.2135, 0.2483, 2.7962, 1.8437, 4.7545, 7.5421, 4.5731, 29.3841, 4.4786, 112.4579,	//Am
			1.2937, 0.2638, 3.1100, 2.0341, 5.0393, 8.7101, 4.7546, 35.2992, 3.5031, 109.4972,	//Cm
			1.2915, 0.2611, 3.1023, 2.0023, 4.9309, 8.4377, 4.6009, 34.1559, 3.4661, 105.8911,	//Bk
			1.2089, 0.2421, 2.7391, 1.7487, 4.3482, 6.7262, 4.0047, 23.2153, 4.6497, 80.3108,	//Cf
			// Atomic groups:
			0.1796, 73.76, 0.8554, 5.399, 1.75, 27.15, 0.05001, 0.1116, 0.2037, 1.062,			//CH
			0.1575, 89.04, 0.8528, 4.637, 2.359, 30.92, 0.00496, -0.344, 0.1935, 0.6172,		// CH2
			0.4245, 4.092, 0.4256, 4.094, 0.2008, 74.32, 2.884, 33.65, 0.16, 0.4189,			// CH3
			0.1568, 64.9, 0.222, 1.017, 0.8391, 4.656, 1.469, 23.17, 0.05579, 0.11,				// NH
			1.991, 25.94, 0.2351, 74.54, 0.8575, 3.893, 5.336, 0.3422, -5.147, 0.3388,			// NH2
			-0.1646, 168.7, 0.2896, 147.3, 0.838, 3.546, 0.1736, 0.4059, 2.668, 29.57,			// NH3
			0.1597, 53.82, 0.2445, 0.7846, 0.8406, 4.042, 1.235, 20.92, 0.03234, -0.01414,		// OH
			-78.51, 9.013, 80.62, 9.014, 0.6401, 1.924, 2.665, 37.71, 0.2755, 0.2941,			// SH
			// Ions that had no x-ray form factors:
			0.0421, 0.0609, 0.210, 0.559, 0.852, 2.96, 1.82, 11.5, 1.17, 37.7,					// O-2
			0.132, 0.109, 0.292, 0.695, 0.703, 2.39, 0.692, 5.65, 0.0959, 14.7					//Cr+4
			;

#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif
#pragma endregion


	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::getAtomsAndCoords(std::vector<float>& xOut, std::vector<float>& yOut, std::vector<float>& zOut, std::vector<u8>& atomInd) const {
		PDB_READER_ERRS res = PDB_OK;

		xOut.resize(x.size());
		yOut.resize(x.size());
		zOut.resize(x.size());
		atomInd.clear();

		for (size_t i = 0; i < x.size(); i++) {
			xOut[i] = (float)x[i];
			yOut[i] = (float)y[i];
			zOut[i] = (float)z[i];
		}
		atomInd = atmInd;

		return res;
	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::getAtomsAndCoords(std::vector<float>& xOut, std::vector<float>& yOut, std::vector<float>& zOut, std::vector<short>& atomInd) const {
		PDB_READER_ERRS res = PDB_OK;
// Templated code, produces conversion errors
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4244)
#endif
		xOut.resize(x.size());		std::copy(x.begin(), x.end(), xOut.begin());
		yOut.resize(x.size());		std::copy(y.begin(), y.end(), yOut.begin());
		zOut.resize(x.size());		std::copy(z.begin(), z.end(), zOut.begin());
		atomInd.resize(x.size());	std::copy(atmInd.begin(), atmInd.end(), atomInd.begin());
#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif

		return res;
	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::writeCondensedFile(string filename) {
		throw std::exception(/*"IMPLEMENT CONDENSED VERSIONS TO DEAL WITH FLOAT/DOUBLE"*/);
		if (x.size() != y.size() ||
			x.size() != z.size() ||
			x.size() != ionInd.size())
			return MISMATCHED_DATA;
		int version = 0;
		size_t sz = x.size();

		fs::path pt(filename);
		if (is_directory(pt)) {
			return FILE_ERROR;
		}

		fs::ofstream of(pt);

		if (!of.good()) {
			of.close();
			return FILE_ERROR;
		}

		of << version << '\t' << sz << '\n';

		for (size_t i = 0; i < sz; i++)
			of << ionInd[i] << '\t' << x[i] << '\t' << y[i] << '\t' << z[i] << '\n';

		if (of.bad()) {
			of.close();
			return FILE_ERROR;
		}

		of.close();

		return PDB_OK;

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::readCondensedFile(string filename) {
		throw std::exception(/*"IMPLEMENT CONDENSED VERSIONS TO DEAL WITH FLOAT/DOUBLE"*/);
		return GENERAL_ERROR;
		// 	fs::path pt(filename);
		// 	if(!exists(pt) || is_directory(pt) || !is_regular_file(pt)) {
		// 		return NO_FILE;
		// 	}
		// 	fs::ifstream inF(pt);
		// 	std::vector<FACC> xx, yy, zz;
		// 	std::vector<short> ioI;
		// 	PDB_READER_ERRS er;
		// 
		// 	int version;
		// 	inF >> version;
		// 	if(version == 0) {
		// 		// read version 0 file
		// 		inF.close();
		// 		er = readVersion0(pt, ioI, xx, yy, zz); 
		// 	} else {
		// 		inF.close();
		// 		return ERROR_IN_PDB_FILE;
		// 	}
		// 
		// 	if(er == PDB_OK) {
		// 		x = xx; y = yy; z = zz;
		// 		ionInd = ioI;
		// 		er = ionIndToatmInd();
		// 	}
		// 	return er;

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::ionIndToatmInd() {
		size_t sz = ionInd.size();
		atmInd.resize(sz);

		for (size_t i = 0; i < sz; i++) {
			if (ionInd[i] < 0)
				return BAD_ATOM;
			if (ionInd[i] < 1)		// H
				atmInd[i] = 1;
			else if (ionInd[i] < 2)	// He
				atmInd[i] = 2;
			else if (ionInd[i] < 4)	// Li
				atmInd[i] = 3;
			else if (ionInd[i] < 6)	// Be
				atmInd[i] = 4;
			else if (ionInd[i] < 7)	// B
				atmInd[i] = 5;
			else if (ionInd[i] < 8)	// C
				atmInd[i] = 6;
			else if (ionInd[i] < 9)	// N
				atmInd[i] = 7;
			else if (ionInd[i] < 11 || ionInd[i] == 217)	// O
				atmInd[i] = 8;
			else if (ionInd[i] < 13)	// F
				atmInd[i] = 9;
			else if (ionInd[i] < 14)	// Ne
				atmInd[i] = 10;
			else if (ionInd[i] < 16)	// Na
				atmInd[i] = 11;
			else if (ionInd[i] < 18)	// Mg
				atmInd[i] = 12;
			else if (ionInd[i] < 20)	// Al
				atmInd[i] = 13;
			else if (ionInd[i] < 22)	// Si
				atmInd[i] = 14;
			else if (ionInd[i] < 23)	// P
				atmInd[i] = 15;
			else if (ionInd[i] < 24)	// S
				atmInd[i] = 16;
			else if (ionInd[i] < 26)	// Cl
				atmInd[i] = 17;
			else if (ionInd[i] < 27)	// Ar
				atmInd[i] = 18;
			else if (ionInd[i] < 29)	// K
				atmInd[i] = 19;
			else if (ionInd[i] < 31)	// Ca
				atmInd[i] = 20;
			else if (ionInd[i] < 33)	// Sc
				atmInd[i] = 21;
			else if (ionInd[i] < 37)	// Ti
				atmInd[i] = 22;
			else if (ionInd[i] < 41)	// V
				atmInd[i] = 23;
			else if (ionInd[i] < 44 || ionInd[i] == 218)	// Cr
				atmInd[i] = 24;
			else if (ionInd[i] < 48)	// Mn
				atmInd[i] = 25;
			else if (ionInd[i] < 51)	// Fe
				atmInd[i] = 26;
			else if (ionInd[i] < 54)	// Co
				atmInd[i] = 27;
			else if (ionInd[i] < 57)	// Ni
				atmInd[i] = 28;
			else if (ionInd[i] < 60)	// Cu
				atmInd[i] = 29;
			else if (ionInd[i] < 62)	// Zn
				atmInd[i] = 30;
			else if (ionInd[i] < 64)	// Ga
				atmInd[i] = 31;
			else if (ionInd[i] < 66)	// Ge
				atmInd[i] = 32;
			else if (ionInd[i] < 67)	// As
				atmInd[i] = 33;
			else if (ionInd[i] < 68)	// Se
				atmInd[i] = 34;
			else if (ionInd[i] < 70)	// Br
				atmInd[i] = 35;
			else if (ionInd[i] < 71)	// Kr
				atmInd[i] = 36;
			else if (ionInd[i] < 73)	// Rb
				atmInd[i] = 37;
			else if (ionInd[i] < 75)	// Sr
				atmInd[i] = 38;
			else if (ionInd[i] < 77)	// Y
				atmInd[i] = 39;
			else if (ionInd[i] < 79)	// Zr
				atmInd[i] = 40;
			else if (ionInd[i] < 82)	// Nb
				atmInd[i] = 41;
			else if (ionInd[i] < 86)	// Mo
				atmInd[i] = 42;
			else if (ionInd[i] < 87)	// Tc
				atmInd[i] = 43;
			else if (ionInd[i] < 90)	// Ru
				atmInd[i] = 44;
			else if (ionInd[i] < 93)	// Rh
				atmInd[i] = 45;
			else if (ionInd[i] < 96)	// Pd
				atmInd[i] = 46;
			else if (ionInd[i] < 99)	// Ag
				atmInd[i] = 47;
			else if (ionInd[i] < 101)	// Cd
				atmInd[i] = 48;
			else if (ionInd[i] < 103)	// In
				atmInd[i] = 49;
			else if (ionInd[i] < 106)	// Sn
				atmInd[i] = 50;
			else if (ionInd[i] < 109)	// Sb
				atmInd[i] = 51;
			else if (ionInd[i] < 110)	// Te
				atmInd[i] = 52;
			else if (ionInd[i] < 112)	// I
				atmInd[i] = 53;
			else if (ionInd[i] < 113)	// Xe
				atmInd[i] = 54;
			else if (ionInd[i] < 115)	// Cs
				atmInd[i] = 55;
			else if (ionInd[i] < 117)	// Ba
				atmInd[i] = 56;
			else if (ionInd[i] < 119)	// La
				atmInd[i] = 57;
			else if (ionInd[i] < 122)	// Ce
				atmInd[i] = 58;
			else if (ionInd[i] < 125)	// Pr
				atmInd[i] = 59;
			else if (ionInd[i] < 127)	// Nd
				atmInd[i] = 60;
			else if (ionInd[i] < 129)	// Pm
				atmInd[i] = 61;
			else if (ionInd[i] < 131)	// Sm
				atmInd[i] = 62;
			else if (ionInd[i] < 134)	// Eu
				atmInd[i] = 63;
			else if (ionInd[i] < 136)	// Gd
				atmInd[i] = 64;
			else if (ionInd[i] < 138)	// Tb
				atmInd[i] = 65;
			else if (ionInd[i] < 140)	// Dy
				atmInd[i] = 66;
			else if (ionInd[i] < 142)	// Ho
				atmInd[i] = 67;
			else if (ionInd[i] < 144)	// Er
				atmInd[i] = 68;
			else if (ionInd[i] < 145)	// Tm
				atmInd[i] = 69;
			else if (ionInd[i] < 149)	// Yb
				atmInd[i] = 70;
			else if (ionInd[i] < 151)	// Lu
				atmInd[i] = 71;
			else if (ionInd[i] < 153)	// Hf
				atmInd[i] = 72;
			else if (ionInd[i] < 155)	// Ta
				atmInd[i] = 73;
			else if (ionInd[i] < 157)	// W
				atmInd[i] = 74;
			else if (ionInd[i] < 158)	// Re
				atmInd[i] = 75;
			else if (ionInd[i] < 160)	// Os
				atmInd[i] = 76;
			else if (ionInd[i] < 163)	// Ir
				atmInd[i] = 77;
			else if (ionInd[i] < 166)	// Pt
				atmInd[i] = 78;
			else if (ionInd[i] < 169)	// Au
				atmInd[i] = 79;
			else if (ionInd[i] < 172)	// Hg
				atmInd[i] = 80;
			else if (ionInd[i] < 175)	// Tl
				atmInd[i] = 81;
			else if (ionInd[i] < 178)	// Pb
				atmInd[i] = 82;
			else if (ionInd[i] < 181)	// Bi
				atmInd[i] = 83;
			else if (ionInd[i] < 182)	// Po
				atmInd[i] = 84;
			else if (ionInd[i] < 183)	// At
				atmInd[i] = 85;
			else if (ionInd[i] < 184)	// Rn
				atmInd[i] = 86;
			else if (ionInd[i] < 185)	// Fr
				atmInd[i] = 87;
			else if (ionInd[i] < 187)	// Ra
				atmInd[i] = 88;
			else if (ionInd[i] < 189)	// Ac
				atmInd[i] = 89;
			else if (ionInd[i] < 191)	// Th
				atmInd[i] = 90;
			else if (ionInd[i] < 192)	// Pa
				atmInd[i] = 91;
			else if (ionInd[i] < 196)	// U
				atmInd[i] = 92;
			else if (ionInd[i] < 200)	// Np
				atmInd[i] = 93;
			else if (ionInd[i] < 204)	// Pu
				atmInd[i] = 94;
			else if (ionInd[i] < 205)	// Am
				atmInd[i] = 95;
			else if (ionInd[i] < 206)	// Cm
				atmInd[i] = 96;
			else if (ionInd[i] < 207)	// Bk
				atmInd[i] = 97;
			else if (ionInd[i] < 208)	// Cf
				atmInd[i] = 98;
			else
				return BAD_ATOM;
		}

		return PDB_OK;
	}

	template<class FLOAT_TYPE>
	bool PDBReader::PDBReaderOb<FLOAT_TYPE>::getHasAnomalous()
	{
		return this->haveAnomalousAtoms;
	}

	template<class FLOAT_TYPE>
	bool PDBReaderOb<FLOAT_TYPE>::getBOnlySolvent() {
		return this->bOnlySolvent;
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::setBOnlySolvent(bool bSolv) {
		this->bOnlySolvent = bSolv;
	}
	template<class FLOAT_TYPE>
	ATOM_RADIUS_TYPE PDBReaderOb<FLOAT_TYPE>::GetRadiusType() {
		return this->atmRadType;
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::SetRadiusType(ATOM_RADIUS_TYPE type) {
		this->atmRadType = type;

		switch (type) {
// Templated code, assignments produce conversion warnings
#if defined(WIN32) || defined(_WIN32)
#pragma warning( push )
#pragma warning( disable : 4305)
#endif
		case RAD_DUMMY_ATOMS_ONLY:
		case RAD_DUMMY_ATOMS_RADII:
			this->rad = &(this->svgRad);
			break;
		case RAD_EMP:
			this->rad = &(this->empRad);
			this->empRad[6] = 0.070;	// Emp
			break;
		case RAD_CALC:
			this->rad = &(this->calcRad);
			break;
		case RAD_VDW:
		default:
			this->rad = &(this->vdwRad);
			break;
		}
#if defined(WIN32) || defined(_WIN32)
#pragma warning( pop )
#endif

	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::CopyPDBData(const PDBReaderOb& src) {
		std::vector<float> xi, yi, zi;
		std::vector<u8> ati;
		PDB_READER_ERRS err = (src).getAtomsAndCoords(xi, yi, zi, ati);

		if (!err) {
			size_t sz = xi.size();
			this->x.resize(sz);
			this->y.resize(sz);
			this->z.resize(sz);
			this->atmInd.resize(sz);
			for (unsigned int i = 0; i < sz; i++) {
				this->x[i] = xi[i];
				this->y[i] = yi[i];
				this->z[i] = zi[i];
				this->atmInd[i] = ati[i];
			}
		}

		return err;
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::BuildPDBFromList(std::vector<std::vector<FLOAT_TYPE> > &FormListData) {
		if (haveAnomalousAtoms)
			throw std::runtime_error("This hasn't been fixed to deal with Anomalous scattering.");
		//Build rotation matrices
		Eigen::Matrix<FLOAT_TYPE, 3, 3, 0, 3, 3> rot;
		std::vector< Eigen::Matrix<FLOAT_TYPE, 3, 3, 0, 3, 3> > RotMat;
		for (unsigned int i = 0; i<(FormListData[1]).size(); i++) {
			RotMat.push_back(EulerD<FLOAT_TYPE>(Radian(FormListData[0][i] * M_PI / 180.0),
				Radian(FormListData[1][i] * M_PI / 180.0),
				Radian(FormListData[2][i] * M_PI / 180.0)));
		}

		vector<FLOAT_TYPE> x_t, y_t, z_t, temperatureFactor_t, occupancy_t;
		vector<string> pdbAtomSerNo_t;
		vector<string> pdbAtomName_t;
		vector<string> pdbResName_t;
		vector<char>   pdbChain_t;
		vector<string> pdbResNo_t;
		vector<string> pdbSegID_t;
		vector<unsigned long int> tter, tmdl;

		vector<string> atom_t;

		tter = ter;
		if (mdl.size() == 0) {
			mdl.push_back(0);
		}
		tmdl = mdl;

		ter.resize(ter.size() * (FormListData[1]).size());
		mdl.resize(mdl.size() * (FormListData[1]).size());

		// FOR LOOP ON ALL THE LINES
		for (size_t i = 0; i < (FormListData[1]).size(); i++){
			//	For loop on all the atoms
			for (size_t atn = 0; atn < (this->x).size(); atn++){
				//		read current x,y,z and make them a vector
				Eigen::Matrix<FLOAT_TYPE, 3, 1, 0, 3, 1> R(FormListData[3][i], FormListData[4][i], FormListData[5][i]), r(this->x[atn], this->y[atn], this->z[atn]);
				//		rotate that vector and add translation
				r = RotMat[i] * r + R;
				//		write new atom in new place
				atom_t.push_back(this->atom[atn]);
				pdbAtomSerNo_t.push_back(this->pdbAtomSerNo[atn]);
				pdbAtomName_t.push_back(this->pdbAtomName[atn]);
				pdbResName_t.push_back(this->pdbResName[atn]);
				pdbChain_t.push_back(this->pdbChain[atn]);
				pdbResNo_t.push_back(this->pdbResNo[atn]);
				pdbSegID_t.push_back(this->pdbSegID[atn]);
				x_t.push_back(r[0]);
				y_t.push_back(r[1]);
				z_t.push_back(r[2]);
				occupancy_t.push_back(this->occupancy[atn]);
				temperatureFactor_t.push_back(this->BFactor[atn]);
			}
			for (size_t tt = 0; tt < tter.size(); tt++) {
				ter[tt + i * tter.size()] = tter[tt] + (unsigned long)(i * x.size());
			}
			for (size_t mm = 0; mm < tmdl.size(); mm++) {
				mdl[mm + i * tmdl.size()] = tmdl[mm] + (unsigned long)(i * x.size());
			}
		}


		this->pdbAtomSerNo = pdbAtomSerNo_t;
		this->pdbAtomName = pdbAtomName_t;
		this->pdbResName = pdbResName_t;
		this->pdbChain = pdbChain_t;
		this->pdbResNo = pdbResNo_t;
		this->pdbSegID = pdbSegID_t;

		this->x = x_t;
		this->y = y_t;
		this->z = z_t;
		this->atom = atom_t;
		this->BFactor = temperatureFactor_t;
		this->occupancy = occupancy_t;

		this->numberOfAA = 0;
		for (unsigned int i = 0; i < this->pdbAtomName.size(); i++) {
			if (boost::iequals(this->pdbAtomName[i], "CA  ") ||
				boost::iequals(this->pdbAtomName[i], " CA ") ||
				boost::iequals(this->pdbAtomName[i], "  CA")) {
				this->numberOfAA++;
			} // if
		} // for


	}

	template<class FLOAT_TYPE>
	PDB_READER_ERRS PDBReaderOb<FLOAT_TYPE>::WritePDBToFile(std::string fileName, const std::stringstream& header) {
		if (haveAnomalousAtoms)
			throw std::runtime_error("This hasn't been fixed to deal with Anomalous scattering.");
		std::ifstream inFile(fileName.c_str());
		fs::path a1, a2, a3;
		fs::path pathName(fileName), nfnm;
		pathName = fs::system_complete(fileName);

		if (!fs::exists(pathName.parent_path())) {
			boost::system::error_code er;
			if (!fs::create_directories(pathName.parent_path(), er)) {
				while (!fs::exists(pathName.parent_path())) {
					pathName = pathName.parent_path();
				}
				pathName = fs::path(pathName.string() + "ERROR_CREATING_DIR");
				{fs::ofstream f(pathName); }
				return FILE_ERROR;
			}
		}

		a1 = pathName.parent_path();
		a2 = fs::path(pathName.stem());
		//a3 = pathName.extension();
		a3 = "pdb";
		nfnm = (a1 / a2).replace_extension(a3);
		std::ofstream outFile(nfnm.string().c_str());
		string line;
		string name;//, atom;
		unsigned int terInd = 0, mdlInd = 0;

		char buff[100];
		time_t now = time(0);
		strftime(buff, 100, "%Y-%m-%d %H:%M:%S", localtime(&now));

		outFile << "REMARK    This PDB file was written with PDBReader on " << buff << "\n";
		outFile << this->pdbPreBla;
		outFile << header.str() << "\n";

		for (unsigned int pp = 0; pp < x.size(); pp++) {
			if (terInd < ter.size() && pp == this->ter[terInd]) {
				outFile << "TER   " << "                      " << "\n";
				terInd++;
			}
			if (mdlInd < mdl.size() && pp == this->mdl[mdlInd]) {

				if (mdlInd > 0) {
					outFile << "ENDMDL" << "\n";
				}
				if (mdlInd < mdl.size()) {
					string mdSt;
					mdSt.resize(4);
					sprintf(&mdSt[0], "%4d", mdlInd + 1);
					outFile << "MODEL " << "    " << mdSt << "\n";
				}
				mdlInd++;
			}
			string lnn, xSt, ySt, zSt, name, occSt, tempSt;
			name = "ATOM  ";
			lnn.resize(80);
			xSt.resize(24);
			ySt.resize(24);
			zSt.resize(24);
			occSt.resize(8);
			tempSt.resize(8);
			sprintf(&xSt[0], "%8f", x[pp] * 10.0);
			sprintf(&ySt[0], "%8f", y[pp] * 10.0);
			sprintf(&zSt[0], "%8f", z[pp] * 10.0);
			sprintf(&occSt[0], "%6f", occupancy[pp]);
			sprintf(&tempSt[0], "%6f", BFactor[pp]);
			//		sprintf(&ln2[0], "%s%s%s%s%s", line.substr(0,30).c_str(), xSt.substr(0,8).c_str(),
			//						ySt.substr(0,8).c_str(), zSt.substr(0,8).c_str(), line.substr(54,line.length()-1).c_str());
			sprintf(&lnn[0], "%s%5s %4s%c%3s %c%4s%c   %s%s%s%s%s%s%s%s", "ATOM  ", pdbAtomSerNo[pp].c_str(), pdbAtomName[pp].c_str(), ' ', pdbResName[pp].c_str(),
				this->pdbChain[pp], this->pdbResNo[pp].c_str(), ' ',
				xSt.substr(0, 8).c_str(), ySt.substr(0, 8).c_str(), zSt.substr(0, 8).c_str(), occSt.substr(0, 6).c_str(),
				tempSt.substr(0, 6).c_str(), "      ", pdbSegID[pp].c_str(), atom[pp].c_str());
			outFile << lnn << "\n";
		}
		if (this->ter.size() > 0) {
			outFile << "TER   " << "                      " << "\n";
		}
		if (this->mdl.size() > 0) {
			outFile << "ENDMDL" << "\n";
		}

		inFile.close();
		outFile.close();

		return PDB_OK;
	}

	template <class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::getAtomIonIndices(string atmAsString, u8& atmInd, u8& ionInd)
	{
		static const std::map<std::string, std::pair<u8, u8>> indice_map = {
			{ " h  ", { 1, 1 } },
			{ "he  ", { 2, 2 } },
			{ "li  ", { 3, 3 } },
			{ "li+1", { 3, 4 } }, { "li1+", { 3, 4 } },
			{ "be  ", { 4, 5 } },
			{ "be+2", { 4, 6 } }, { "be2+", { 4, 6 } },
			{ " b  ", { 5, 7 } },
			{ " c  ", { 6, 8 } },
			{ " n  ", { 7, 9 } }, { " n1+", { 7, 9 } }, { " n+1", { 7, 9 } },
			{ " o  ", { 8, 10 } },
			{ " o-1", { 8, 11 } }, { " o1-", { 8, 11 } },
			{ " o-2", { 8, 217 } }, { " o2-", { 8, 217 } },
			{ " f  ", { 9, 12 } },
			{ " f-1", { 9, 13 } }, { " f1-", { 9, 13 } },
			{ "ne  ", { 10, 14 } },
			{ "na  ", { 11, 15 } },
			{ "na+1", { 11, 16 } }, { "na1+", { 11, 16 } },
			{ "mg  ", { 12, 17 } },
			{ "mg2+", { 12, 18 } }, { "mg+2", { 12, 18 } },
			{ "al  ", { 13, 19 } },
			{ "al3+", { 13, 20 } }, { "al+3", { 13, 20 } },
			{ "si  ", { 14, 21 } },
			{ "si+4", { 14, 22 } }, { "si4+", { 14, 22 } },
			{ " p  ", { 15, 23 } },
			{ " s  ", { 16, 24 } },
			{ "cl  ", { 17, 25 } },
			{ "cl-1", { 17, 26 } }, { "cl1-", { 17, 26 } },
			{ "ar  ", { 18, 27 } },
			{ " k  ", { 19, 28 } },
			{ " k+1", { 19, 29 } }, { " k1+", { 19, 29 } },
			{ "ca  ", { 20, 30 } },
			{ "ca+2", { 20, 31 } }, { "ca2+", { 20, 31 } },
			{ "sc  ", { 21, 32 } },
			{ "sc+3", { 21, 33 } }, { "sc3+", { 21, 33 } },
			{ "ti  ", { 22, 34 } },
			{ "ti+2", { 22, 35 } }, { "ti2+", { 22, 35 } },
			{ "ti+3", { 22, 36 } }, { "ti3+", { 22, 36 } },
			{ "ti+4", { 22, 37 } }, { "ti4+", { 22, 37 } },
			{ " v  ", { 23, 38 } },
			{ " v+2", { 23, 39 } }, { " v2+", { 23, 39 } },
			{ " v+3", { 23, 40 } }, { " v3+", { 23, 40 } },
			{ " v+5", { 23, 41 } }, { " v5+", { 23, 41 } },
			{ "cr  ", { 24, 42 } },
			{ "cr+2", { 24, 43 } }, { "cr2+", { 24, 43 } },
			{ "cr+3", { 24, 44 } }, { "cr3+", { 24, 44 } },
			{ "cr+4", { 24, 218 } }, { "cr4+", { 24, 218 } },
			{ "mn  ", { 25, 45 } },
			{ "mn+2", { 25, 46 } }, { "mn2+", { 25, 46 } },
			{ "mn+3", { 25, 47 } }, { "mn3+", { 25, 47 } },
			{ "mn+4", { 25, 48 } }, { "mn4+", { 25, 48 } },
			{ "fe  ", { 26, 49 } },
			{ "fe+2", { 26, 50 } }, { "fe2+", { 26, 50 } },
			{ "fe+3", { 26, 51 } }, { "fe3+", { 26, 51 } },
			{ "co  ", { 27, 52 } },
			{ "co+2", { 27, 53 } }, { "co2+", { 27, 53 } },
			{ "co+3", { 27, 54 } }, { "co3+", { 27, 54 } },
			{ "ni  ", { 28, 55 } },
			{ "ni+2", { 28, 56 } }, { "ni2+", { 28, 56 } },
			{ "ni+3", { 28, 57 } }, { "ni3+", { 28, 57 } },
			{ "cu  ", { 29, 58 } },
			{ "cu+1", { 29, 59 } }, { "cu1+", { 29, 59 } },
			{ "cu+2", { 29, 60 } }, { "cu2+", { 29, 60 } },
			{ "zn  ", { 30, 61 } },
			{ "zn+2", { 30, 62 } }, { "zn2+", { 30, 62 } },
			{ "ga  ", { 31, 63 } },
			{ "ga+3", { 31, 64 } }, { "ga3+", { 31, 64 } },
			{ "ge  ", { 32, 65 } },
			{ "ge+4", { 32, 66 } }, { "ge4+", { 32, 66 } },
			{ "as  ", { 33, 67 } },
			{ "se  ", { 34, 68 } },
			{ "br  ", { 35, 69 } },
			{ "br-1", { 35, 70 } }, { "br1-", { 35, 70 } },
			{ "kr  ", { 36, 71 } },
			{ "rb  ", { 37, 72 } },
			{ "rb+1", { 37, 73 } }, { "rb1+", { 37, 73 } },
			{ "sr  ", { 38, 74 } },
			{ "sr+2", { 38, 75 } }, { "sr2+", { 38, 75 } },
			{ " y  ", { 39, 76 } },
			{ " y+3", { 39, 77 } }, { " y3+", { 39, 77 } },
			{ "zr  ", { 40, 78 } },
			{ "zr+4", { 40, 79 } }, { "zr4+", { 40, 79 } },
			{ "nb  ", { 41, 80 } },
			{ "nb+3", { 41, 81 } }, { "nb3+", { 41, 81 } },
			{ "nb+5", { 41, 82 } }, { "nb5+", { 41, 82 } },
			{ "mo  ", { 42, 83 } },
			{ "mo+3", { 42, 84 } }, { "mo3+", { 42, 84 } },
			{ "mo+5", { 42, 85 } }, { "mo5+", { 42, 85 } },
			{ "mo+6", { 42, 86 } }, { "mo6+", { 42, 86 } },
			{ "tc  ", { 43, 87 } },
			{ "ru  ", { 44, 88 } },
			{ "ru+3", { 44, 89 } }, { "ru3+", { 44, 89 } },
			{ "ru+4", { 44, 90 } }, { "ru4+", { 44, 90 } },
			{ "rh  ", { 45, 91 } },
			{ "rh+3", { 45, 92 } }, { "rh3+", { 45, 92 } },
			{ "rh+4", { 45, 93 } }, { "rh4+", { 45, 93 } },
			{ "pd  ", { 46, 94 } },
			{ "pd+2", { 46, 95 } }, { "pd2+", { 46, 95 } },
			{ "pd+4", { 46, 96 } }, { "pd4+", { 46, 96 } },
			{ "ag  ", { 47, 97 } },
			{ "ag+1", { 47, 98 } }, { "ag1+", { 47, 98 } },
			{ "ag+2", { 47, 99 } }, { "ag2+", { 47, 99 } },
			{ "cd  ", { 48, 100 } },
			{ "cd+2", { 48, 101 } }, { "cd2+", { 48, 101 } },
			{ "in  ", { 49, 102 } },
			{ "in+3", { 49, 103 } }, { "in3+", { 49, 103 } },
			{ "sn  ", { 50, 104 } },
			{ "sn+2", { 50, 105 } }, { "sn2+", { 50, 105 } },
			{ "sn+4", { 50, 106 } }, { "sn4+", { 50, 106 } },
			{ "sb  ", { 51, 107 } },
			{ "sb+3", { 51, 108 } }, { "sb3+", { 51, 108 } },
			{ "sb+5", { 51, 109 } }, { "sb5+", { 51, 109 } },
			{ "te  ", { 52, 110 } },
			{ " i  ", { 53, 111 } },
			{ " i-1", { 53, 112 } }, { " i1-", { 53, 112 } },
			{ "xe  ", { 54, 113 } },
			{ "cs  ", { 55, 114 } },
			{ "cs+1", { 55, 115 } }, { "cs1+", { 55, 115 } },
			{ "ba  ", { 56, 116 } },
			{ "ba+2", { 56, 117 } }, { "ba2+", { 56, 117 } },
			{ "la  ", { 57, 118 } },
			{"la+3", { 57, 119 }}, { "la3+", { 57, 119 } },
			{ "ce  ", { 58, 120 } },
			{ "ce+3", { 58, 121 } }, { "ce3+", { 58, 121 } },
			{ "ce+4", { 58, 122 } }, { "ce4+", { 58, 122 } },
			{ "pr  ", { 59, 123 } },
			{ "pr+3", { 59, 124 } }, { "pr3+", { 59, 124 } },
			{ "pr+4", { 59, 125 } }, { "pr4+", { 59, 125 } },
			{ "nd  ", { 60, 126 } },
			{ "nd+3", { 60, 127 } }, { "nd3+", { 60, 127 } },
			{ "pm  ", { 61, 128 } },
			{ "pm+3", { 61, 129 } }, { "pm3+", { 61, 129 } },
			{ "sm  ", { 62, 130 } },
			{ "sm+1", { 62, 131 } }, { "sm1+", { 62, 131 } },
			{ "eu  ", { 63, 132 } },
			{ "eu+2", { 63, 133 } }, { "eu2+", { 63, 133 } },
			{ "eu+3", { 63, 134 } }, { "eu3+", { 63, 134 } },
			{ "gd  ", { 64, 135 } },
			{ "gd+3", { 64, 136 } }, { "gd3+", { 64, 136 } },
			{ "tb  ", { 65, 137 } },
			{ "tb+3", { 65, 138 } }, { "tb3+", { 65, 138 } },
			{ "dy  ", { 66, 139 } },
			{ "dy+3", { 66, 140 } }, { "dy3+", { 66, 140 } },
			{ "ho  ", { 67, 141 } },
			{ "ho+3", { 67, 142 } }, { "ho3+", { 67, 142 } },
			{ "er  ", { 68, 143 } },
			{ "er+3", { 68, 144 } }, { "er3+", { 68, 144 } },
			{ "tm  ", { 69, 145 } },
			{ "tm+3", { 69, 146 } }, { "tm3+", { 69, 146 } },
			{ "yb  ", { 70, 147 } },
			{ "yb+2", { 70, 148 } }, { "yb2+", { 70, 148 } },
			{ "yb+3", { 70, 149 } }, { "yb3+", { 70, 149 } },
			{ "lu  ", { 71, 150 } },
			{ "lu+3", { 71, 151 } }, { "lu3+", { 71, 151 } },
			{ "hf  ", { 72, 152 } },
			{ "hf+4", { 72, 153 } }, { "hf4+", { 72, 153 } },
			{ "ta  ", { 73, 154 } },
			{ "ta+5", { 73, 155 } }, { "ta5+", { 73, 155 } },
			{ " w  ", { 74, 156 } },
			{ " w+6", { 74, 157 } }, { " w6+", { 74, 157 } },
			{ "re  ", { 75, 158 } },
			{ "os  ", { 76, 159 } },
			{ "os+4", { 76, 160 } }, { "os4+", { 76, 160 } },
			{ "ir  ", { 77, 161 } },
			{ "ir+3", { 77, 162 } }, { "ir3+", { 77, 162 } },
			{ "ir+4", { 77, 163 } }, { "ir4+", { 77, 163 } },
			{ "pt  ", { 78, 164 } },
			{ "pt+2", { 78, 165 } }, { "pt2+", { 78, 165 } },
			{ "pt+4", { 78, 166 } }, { "pt4+", { 78, 166 } },
			{ "au  ", { 79, 167 } },
			{ "au+1", { 79, 168 } }, { "au1+", { 79, 168 } },
			{ "au+3", { 79, 169 } }, { "au3+", { 79, 169 } },
			{ "hg  ", { 80, 170 } },
			{ "hg+1", { 80, 171 } }, { "hg1+", { 80, 171 } },
			{ "hg+2", { 80, 172 } }, { "hg2+", { 80, 172 } },
			{ "tl  ", { 81, 173 } },
			{ "tl+1", { 81, 174 } }, { "tl1+", { 81, 174 } },
			{ "tl+3", { 81, 175 } }, { "tl3+", { 81, 175 } },
			{ "pb  ", { 82, 176 } },
			{ "pb+2", { 82, 177 } }, { "pb2+", { 82, 177 } },
			{ "pb+4", { 82, 178 } }, { "pb4+", { 82, 178 } },
			{ "bi  ", { 83, 179 } },
			{ "bi+3", { 83, 180 } }, { "bi3+", { 83, 180 } },
			{ "bi+5", { 83, 181 } }, { "bi5+", { 83, 181 } },
			{ "po  ", { 84, 182 } },
			{ "at  ", { 85, 183 } },
			{ "rn  ", { 86, 184 } },
			{ "fr  ", { 87, 185 } },
			{ "ra  ", { 88, 186 } },
			{ "ra+2", { 88, 187 } }, { "ra2+", { 88, 187 } },
			{ "ac  ", { 89, 188 } },
			{ "ac+3", { 89, 189 } }, { "ac3+", { 89, 189 } },
			{ "th  ", { 90, 190 } },
			{ "th+4", { 90, 191 } }, { "th4+", { 90, 191 } },
			{ "pa  ", { 91, 192 } },
			{ " u  ", { 92, 193 } },
			{ " u+3", { 92, 194 } }, { " u3+", { 92, 194 } },
			{ " u+4", { 92, 195 } }, { " u4+", { 92, 195 } },
			{ " u+6", { 92, 196 } }, { " u6+", { 92, 196 } },
			{ "np  ", { 93, 197 } },
			{ "np+3", { 93, 198 } }, { "np3+", { 93, 198 } },
			{ "np+4", { 93, 199 } }, { "np4+", { 93, 199 } },
			{ "np+6", { 93, 200 } }, { "np6+", { 93, 200 } },
			{ "pu  ", { 94, 201 } },
			{ "pu+3", { 94, 202 } }, { "pu3+", { 94, 202 } },
			{ "pu+4", { 94, 203 } }, { "pu4+", { 94, 203 } },
			{ "pu+6", { 94, 204 } }, { "pu6+", { 94, 204 } },
			{ "am  ", { 95, 205 } },
			{ "cm  ", { 96, 206 } },
			{ "bk  ", { 97, 207 } },
			{ "cf  ", { 98, 208 } },

			// Atomic groups (CH2, OH, etc.)
			{ "ch", { 119, 209} },
			{ "ch2", { 120, 210} },
			{ "ch3", { 121, 211} },
			{ "nh", { 122, 212} },
			{ "nh2", { 123, 213} },
			{ "nh3", { 124, 214} },
			{ "oh", { 125, 215} },
			{ "sh", { 126, 216} }
		};
		std::string lower_case = atmAsString.c_str();
		
		lower_case[0] = std::tolower(lower_case[0], std::locale::classic());
		lower_case[1] = std::tolower(lower_case[1], std::locale::classic());

		auto it = indice_map.find(lower_case);
		
		if(it == indice_map.end())
		{
			// TODO: Deal with bad atoms properly
			throw pdbReader_exception(ERROR_IN_PDB_FILE, ("No atom matches type: \"" + atmAsString + "\"\n").c_str());
		}

		atmInd = it->second.first;
		ionInd = it->second.second;
	}


	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::ExtractRelevantCoeffs(std::vector< IonEntry<FLOAT_TYPE> > &entries,
		u64 vecSize, std::vector<u8>& sIonInd, std::vector<u8>& sAtmInd, std::vector<int>& atmsPerIon,
		std::vector<unsigned char>& sortedCoefIonInd, std::vector<FLOAT_TYPE>& sX,
		std::vector<FLOAT_TYPE>& sY, std::vector<FLOAT_TYPE>& sZ, std::vector<PDBReader::float4>& atmLocs,
		std::vector<float> sBFactor, std::vector<FLOAT_TYPE>& sCoeffs, std::vector<std::complex<float>>* sfPrimes/*= NULL*/
	)
	{
		if (entries.size() == 0)
		{
			return;
		}

		std::vector<short> iMapVec;

		int apiInd = 0;
		unsigned int lastCumApi = 0;
		u8 curInd = entries[0].ionInd;
		iMapVec.push_back(curInd);
		for (unsigned int i = 0; i < vecSize; i++) {
			sIonInd[i] = entries[i].ionInd;
			sAtmInd[i] = entries[i].atmInd;

			if (sIonInd[i] != curInd)
			{
				atmsPerIon[apiInd] = i - lastCumApi;
				apiInd++;
				curInd = sIonInd[i];
				lastCumApi = i;
				iMapVec.push_back(curInd);
			}
			if (sfPrimes)
				sfPrimes->at(i) = std::complex<FLOAT_TYPE>(entries[i].fPrime, entries[i].fPrimePrime);

			sortedCoefIonInd[i] = apiInd;

			sX[i] = entries[i].x;
			sY[i] = entries[i].y;
			sZ[i] = entries[i].z;

			PDBReader::float4 loc = { float(entries[i].x), float(entries[i].y), float(entries[i].z), 0.f };
			atmLocs[i] = loc;

			sBFactor[i] = float(entries[i].BFactor);
		}

		atmsPerIon[apiInd] = int(vecSize - lastCumApi);

		apiInd++;

		assert(std::accumulate(atmsPerIon.begin(), atmsPerIon.end(), 0) == vecSize);


		// Copy sorted coefficients to a vector
		Eigen::Array<FLOAT_TYPE, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor> newCoeffs;
		Eigen::Map<Eigen::Array<FLOAT_TYPE, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> atmFFcoefsMap(atmFFcoefs.data(), NUMBER_OF_ATOMIC_FORM_FACTORS, 10);

		newCoeffs.resize(apiInd, 10);
		for (int i = 0; i < apiInd; i++) {
			newCoeffs.row(i) = atmFFcoefsMap.row(iMapVec[i]);
		}

		atmsPerIon.resize(apiInd);
		sCoeffs.resize(apiInd * 10);
		for (int i = 0; i < 10; i++)
			memcpy(&sCoeffs[i * apiInd], newCoeffs.col(i).data(), apiInd * sizeof(FLOAT_TYPE));
	}

	template<class FLOAT_TYPE>
	std::string PDBReaderOb<FLOAT_TYPE>::removeSpaces(std::string source)
	{
		std::string result;
		for (auto is = source.begin(); is != source.end(); is++)
			if (!::isspace(*is))
				result += *is;
		return result;
	}


	template<class FLOAT_TYPE>
	std::string PDBReaderOb<FLOAT_TYPE>::removeDigits(std::string source)
	{
		std::string result;
		for (auto is = source.begin(); is != source.end(); is++)
			if (!isdigit(*is))
				result += *is;
		return result;
	}

	template<class FLOAT_TYPE>
	void PDBReaderOb<FLOAT_TYPE>::ChangeResidueGroupsToImplicit(std::string amino_acid_name, const int i, const int aa_size)
	{
		// Assumes pH = 7.4
		static const std::map < std::string/*amino acid name, e.g. "ser"*/,
								std::map < std::string/*atom label, e.g. cg2*/, std::string /*Type of group*/ >>
			residue_atom_hydrogen_number =
		{
			{ "arg", { 
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "cg", "ch2" }, { "cd", "ch2" },
				{ "ne", "nh" }, { "cz", "" }, { "nh", "nh2" }, { "nh", "nh2" }
			} },
			{ "his", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "cg", "" },
				{ "nd", "nh" /* I don't agree with this (there's no added electron), but crysol and FoXS add the proton here */}, { "cd", "ch" }, { "ce", "ch" }, { "ne", "nh" }
			}, },
			{ "lys", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "cg", "ch2" },
				{ "cd", "ch2" }, { "ce", "ch2" }, { "nz", "nh3" }
			} },
			{ "asp", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "cg", "" },
				{ "od", "" }, { "od", ""}
			} },
			{ "glu", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "cg", "ch2" },
				{ "cd", "" }, { "oe", "" }, { "oe", "" }
			} },
			{ "ser", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, 
				{ "og", "oh" }
			} },
			{ "thr", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch" },
				{ "cg", "ch3" }, { "og", "oh" }
			} },
			{ "asn", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "" }, { "od", "" }, { "nd", "nh2" }
			} },
			{ "gln", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "ch2" }, { "cd", "" }, { "oe", "" }, { "ne", "nh2" }
			} },
			{ "cys", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, {"sg", "sh"}
			} },
			{ "sec", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" }, { "seg", "" } // We don't add this hydrogen
			} },
			{ "gly", {
				{ "n", "nh" }, { "ca", "ch2" }, { "c", "" }, { "o", "" }
			} },
			{ "pro", {
				{ "n", "" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "ch2" }, { "cd", "ch2" }
			} },
			{ "ala", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch3" }
			} },
			{ "val", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch" },
				{ "cg", "ch3" }
			} },
			{ "ile", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch" },
				{ "cg1", "ch2" }, { "cg2", "ch3" }, { "cd1", "ch3" }, { "cd", "ch3" } // This is a case where the number matters
			} },
			{ "leu", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "ch" }, { "cd", "ch3" }, { "cd", "ch3" }
			} },
			{ "met", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "ch2" }, { "sd", "" }, { "ce", "ch3" }
			} },
			{ "phe", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "" }, { "cd", "ch" }, { "cd", "ch" }, { "ce", "ch" }, { "ce", "ch" }, { "cz", "ch" }
			} },
			{ "tyr", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "" }, { "cd", "ch" }, { "cd", "ch" }, { "ce", "ch" }, { "ce", "ch" }, { "cz", "" }, { "oh", "oh" }
			} },
			{ "trp", {
				{ "n", "nh" }, { "ca", "ch" }, { "c", "" }, { "o", "" }, { "cb", "ch2" },
				{ "cg", "" }, { "cd1", "ch" }, { "cd2", "" }/*This is a case where the number matters*/, { "ne1", "nh" }, { "ce2", "" }, { "ce3", "ch" }, /*This is a case where the number matters*/
				{ "cz2", "ch" }, { "cz3", "ch" }, { "ch2", "ch" }
			} },
			//{ "pyl", {} },
			/// RNA
			{ "a", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "c4'", "ch" }, { "o4'", "" }, { "c3'", "ch" },
				{ "o3'", "" }, { "c2'", "ch" }, { "o2'", "oh" }, { "c1'", "ch" },

				{ "n9", "" }, { "c8", "ch" }, { "n7", "" },
				{ "c6", "" }, { "c5", "" }, { "n6", "nh2" },
				{ "n1", "" }, { "c2", "ch" }, { "n3", "" }, { "c4", "" }
			} },
			{ "u", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },
				
				{ "o5'", "" }, { "c5'", "ch2" }, { "c4'", "ch" }, { "o4'", "" }, { "c3'", "ch" },
				{ "o3'", "" }, { "c2'", "ch" }, { "o2'", "oh" }, { "c1'", "ch" },
				
				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "nh" },
				{ "c4", "" }, { "o4", "" }, { "c5", "ch" }, { "c6", "ch" },
			} },
			{ "c", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "c4'", "ch" }, { "o4'", "" }, { "c3'", "ch" },
				{ "o3'", "" }, { "c2'", "ch" }, { "o2'", "oh" }, { "c1'", "ch" },

				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "" },
				{ "c4", "" }, { "n4", "nh2" }, { "c5", "ch" }, { "c6", "ch" },
			} },
			{ "g", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "c4'", "ch" }, { "o4'", "" }, { "c3'", "ch" },
				{ "o3'", "" }, { "c2'", "ch" }, { "o2'", "oh" }, { "c1'", "ch" },

				{ "n9", "" }, { "c8", "ch" }, { "n7", "" }, { "c5", "" }, { "c6", "" },
				{ "o6", "" }, { "n1", "nh" }, { "c2", "" }, { "n2", "nh2" }, { "n3", "" }, { "c4", "" },
			} },
			{ "t", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "c4'", "ch" }, { "o4'", "" }, { "c3'", "ch" },
				{ "o3'", "" }, { "c2'", "ch" }, { "o2'", "oh" }, { "c1'", "ch" },

				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "nh" },
				{ "c4", "" }, { "o4", "" }, { "c5", "" }, { "c7", "ch3" }, { "c6", "ch" },
			} },
// 			{ "i", { // TODO
// 			} },
			/// DNA
			{ "da", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "o4'", "" }, { "c4'", "ch" },
				{ "o3'", "" }, { "c3'", "ch" }, { "c2'", "ch2" }, { "c1'", "ch" },

				{ "n9", "" }, { "c8", "ch" }, { "n7", "" },
				{ "c6", "" }, { "c5", "" }, { "n6", "nh2" },
				{ "n1", "" }, { "c2", "ch" }, { "n3", "" }, { "c4", "" }
			} },
			{ "du", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "o4'", "" }, { "c4'", "ch" },
				{ "o3'", "" }, { "c3'", "ch" }, { "c2'", "ch2" }, { "c1'", "ch" },

				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "nh" },
				{ "c4", "" }, { "o4", "" }, { "c5", "ch" }, { "c6", "ch" },
			} },
			{ "dc", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "o4'", "" }, { "c4'", "ch" },
				{ "o3'", "" }, { "c3'", "ch" }, { "c2'", "ch2" }, { "c1'", "ch" },
				
				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "" },
				{ "c4", "" }, { "n4", "nh2" }, { "c5", "ch" }, { "c6", "ch" },
			} },
			{ "dg", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "o4'", "" }, { "c4'", "ch" },
				{ "o3'", "" }, { "c3'", "ch" }, { "c2'", "ch2" }, { "c1'", "ch" },

				{ "n9", "" }, { "c8", "ch" }, { "n7", "" }, { "c5", "" }, { "c6", "" },
				{ "o6", "" }, { "n1", "nh" }, { "c2", "" }, { "n2", "nh2" }, { "n3", "" }, { "c4", "" },			
			} },
			{ "dt", {
				{ "p", "" }, { "op1", "" }, { "op2", "" }, { "op", "" }, { "op3", "" },

				{ "o5'", "" }, { "c5'", "ch2" }, { "o4'", "" }, { "c4'", "ch" },
				{ "o3'", "" }, { "c3'", "ch" }, { "c2'", "ch2" }, { "c1'", "ch" },

				{ "n1", "" }, { "c2", "" }, { "o2", "" }, { "n3", "nh" },
				{ "c4", "" }, { "o4", "" }, { "c5", "" }, { "c7", "ch3" }, { "c6", "ch" },
			} },
// 			{ "di", { // TODO
// 			} },

		};
		// 0) Determine that this is an amino acid we recognize
		amino_acid_name = removeSpaces(amino_acid_name);
		std::transform(amino_acid_name.begin(), amino_acid_name.end(), amino_acid_name.begin(), ::tolower);
		auto aa = residue_atom_hydrogen_number.find(amino_acid_name);
		if (aa == residue_atom_hydrogen_number.end())
		{
			std::cout << amino_acid_name << " is an invalid amino acid name.\n";
			return;
		}
		// 1) Determine that all atoms are correctly denoted by there position (e.g. CD2 - Carbon delta 2)
		vector<string> implicit_atom(aa_size);
		for (int ind = i; ind < i + aa_size; ind++)
		{
			std::string atom_denotion = pdbAtomName[ind];
			atom_denotion = removeSpaces(atom_denotion);
			if (!(amino_acid_name.compare("trp") == 0 || amino_acid_name.compare("ile") == 0 || amino_acid_name.length() < 3))
				// Remove digits except in the cases where they make a difference
				atom_denotion = removeDigits(atom_denotion);
			std::transform(atom_denotion.begin(), atom_denotion.end(), atom_denotion.begin(), ::tolower);
			auto a_in_aa = aa->second.find(atom_denotion);
			if (atom_denotion.compare("oxt") == 0) continue;
			if (a_in_aa == aa->second.end())
			{
				std::cout << atom_denotion << " not found in " << amino_acid_name << "\n";
				return;
			}
			implicit_atom[ind - i] = a_in_aa->second;
		}

		// 2) Modify the member implicitAtoms
		for (int j = 0; j < aa_size; j++)
			implicitAtom[i + j] = implicit_atom[j];

		number_of_implicit_atoms += aa_size;
		number_of_implicit_amino_acids++;
	}

	pdbReader_exception::pdbReader_exception(PDB_READER_ERRS error_code, const char *error_message)
		: _errorCode(error_code), _errorMessage(error_message)
	{
	}

	PDB_READER_ERRS pdbReader_exception::GetErrorCode() const
	{
		return _errorCode;
	}

	std::string pdbReader_exception::GetErrorMessage() const
	{
		return _errorMessage;
	}

	const char* pdbReader_exception::what() const _GLIBCXX_NOEXCEPT { return _errorMessage.c_str(); }


}; // namespace PDBReader
